#include "runtime.h"
#include <inttypes.h>
#include <string.h>
#include <stdlib.h>
#include <sys/time.h>
#include <time.h>
#include "driver/gpio.h"
#include "driver/rtc_io.h"
#include "hal/gpio_ll.h"
#include "soc/gpio_struct.h"
#include "esp_attr.h"
#include "esp_timer.h"
#include "esp_sleep.h"
#include "esp_mac.h"
#include "esp_random.h"
#include "esp_app_desc.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "freertos/task.h"

RTC_DATA_ATTR gr_state_t gr_state;
static RTC_DATA_ATTR gr_daily_t daily;
gr_config_t gr_config;
char gr_device_id[13];
SemaphoreHandle_t gr_state_mutex;
static nvs_handle_t state_nvs;
static bool storage_open;
static bool configuration_loaded;
static bool timezone_initialized;
static adc_oneshot_unit_handle_t adc;
static adc_cali_handle_t calibration;
static gr_debounce_t reed;
static TaskHandle_t counter_task;
static portMUX_TYPE ring_lock = portMUX_INITIALIZER_UNLOCKED;
typedef struct { int64_t us; int level; } edge_t;
#define RING_SIZE 256
static DRAM_ATTR edge_t ring[RING_SIZE];
static DRAM_ATTR unsigned ring_head, ring_tail;
static DRAM_ATTR bool ring_overflow;

int64_t gr_now_us(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (int64_t)tv.tv_sec * 1000000 + tv.tv_usec;
}

static void IRAM_ATTR reed_isr(void *arg) {
    (void)arg;
    edge_t e = {.us = esp_timer_get_time(), .level = gpio_ll_get_level(&GPIO, GR_REED)};
    portENTER_CRITICAL_ISR(&ring_lock);
    unsigned next = (ring_head + 1) % RING_SIZE;
    if (next == ring_tail) ring_overflow = true;
    else { ring[ring_head] = e; ring_head = next; }
    portEXIT_CRITICAL_ISR(&ring_lock);
}

static void record_sample(int level, int64_t us) {
    if (gr_debounce_sample(&reed, level, us)) {
        if (gr_state.pending == UINT64_MAX) gr_state.counter_fault = 1;
        else gr_state.pending++;
    }
    gr_state.reed_level = reed.stable;
}

/* Caller owns state_mutex. Edges are replayed chronologically before the current sample. */
static void drain_counter(void) {
    for (;;) {
        edge_t e;
        portENTER_CRITICAL(&ring_lock);
        bool have = ring_head != ring_tail;
        if (have) { e = ring[ring_tail]; ring_tail = (ring_tail + 1) % RING_SIZE; }
        if (ring_overflow) gr_state.counter_fault = 1;
        portEXIT_CRITICAL(&ring_lock);
        if (!have) break;
        record_sample(e.level, e.us);
    }
    /* Do not sample past an edge that has arrived between the drain and this read. */
    portENTER_CRITICAL(&ring_lock);
    if (ring_head == ring_tail) record_sample(gpio_ll_get_level(&GPIO, GR_REED), esp_timer_get_time());
    portEXIT_CRITICAL(&ring_lock);
}

static void count_task(void *arg) {
    (void)arg;
    for (;;) {
        xSemaphoreTake(gr_state_mutex, portMAX_DELAY);
        drain_counter();
        if (gpio_get_level(GR_BUTTON)) gr_state.button_consumed = 0;
        xSemaphoreGive(gr_state_mutex);
        vTaskDelay(pdMS_TO_TICKS(5));
    }
}

static esp_err_t open_storage(void) {
    if (storage_open) return ESP_OK;
    esp_err_t err = nvs_flash_init();
    if (err != ESP_OK) return err;
    err = nvs_open("gas_reader", NVS_READWRITE, &state_nvs);
    if (err == ESP_OK) storage_open = true;
    return err;
}

esp_err_t gr_load_configuration(void) {
    if (configuration_loaded) return ESP_OK;
    esp_err_t err = open_storage();
    if (err != ESP_OK) return err;
    nvs_handle_t h;
    err = nvs_open("config", NVS_READONLY, &h);
    if (err != ESP_OK) return err;
#define GET_CONFIG(key, field) do { size_t len = sizeof(gr_config.field); \
    err = nvs_get_str(h, key, gr_config.field, &len); \
    if (err != ESP_OK) goto done; } while (0)
    GET_CONFIG("ssid", ssid);
    GET_CONFIG("wifi_password", wifi_password);
    /* Missing optional keys are expected on devices provisioned before fallback support. */
#define GET_OPTIONAL(key, field) do { size_t len = sizeof(gr_config.field); \
    err = nvs_get_str(h, key, gr_config.field, &len); \
    if (err == ESP_ERR_NVS_NOT_FOUND) { gr_config.field[0] = '\0'; err = ESP_OK; } \
    if (err != ESP_OK) goto done; } while (0)
    GET_OPTIONAL("fallback_ssid", fallback_ssid);
    GET_OPTIONAL("fallback_pass", fallback_wifi_password);
    GET_CONFIG("mqtt_uri", mqtt_uri);
    GET_CONFIG("mqtt_user", mqtt_user);
    GET_CONFIG("mqtt_password", mqtt_password);
    GET_CONFIG("ota_password", ota_password);
    if (!gr_config.ssid[0] || strncmp(gr_config.mqtt_uri, "mqtt://", 7) || strlen(gr_config.ota_password) < 12 ||
        (!!gr_config.fallback_ssid[0] != !!gr_config.fallback_wifi_password[0]))
        err = ESP_ERR_INVALID_ARG;
done:
    nvs_close(h);
    if (err == ESP_OK) configuration_loaded = true;
    else memset(&gr_config, 0, sizeof(gr_config));
    return err;
#undef GET_CONFIG
#undef GET_OPTIONAL
}

static esp_err_t write_state(gr_state_t *candidate) {
    esp_err_t err = open_storage();
    if (err != ESP_OK) return err;
    if (candidate->generation == UINT64_MAX) return ESP_ERR_INVALID_STATE;
    candidate->generation++;
    gr_seal(candidate);
    err = nvs_set_blob(state_nvs, "checkpoint", candidate, sizeof(*candidate));
    if (err == ESP_OK) err = nvs_commit(state_nvs);
    return err;
}

esp_err_t gr_runtime_init(bool deep_wake) {
    bool valid_rtc = deep_wake && gr_valid(&gr_state);
    esp_err_t err;
    /* Reed-only wakes do not initialize NVS, networking or ADC. Never auto-erase NVS. */
    if (!valid_rtc && (err = open_storage()) != ESP_OK) return err;
    uint8_t mac[6];
    if ((err = esp_read_mac(mac, ESP_MAC_WIFI_STA)) != ESP_OK) return err;
    snprintf(gr_device_id, sizeof(gr_device_id), "%02x%02x%02x%02x%02x%02x",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    gr_state_mutex = xSemaphoreCreateMutex();
    if (!gr_state_mutex) return ESP_ERR_NO_MEM;
    if (!valid_rtc || !gr_daily_valid(&daily)) gr_daily_init(&daily, gr_now_us());
    if ((err = rtc_gpio_deinit(GR_REED)) != ESP_OK) return err;
    if ((err = rtc_gpio_deinit(GR_BUTTON)) != ESP_OK) return err;
    gpio_config_t inputs = {.pin_bit_mask = (1ULL << GR_REED) | (1ULL << GR_BUTTON),
        .mode = GPIO_MODE_INPUT, .intr_type = GPIO_INTR_DISABLE};
    if ((err = gpio_config(&inputs)) != ESP_OK) return err;
    if ((err = gpio_set_pull_mode(GR_BUTTON, GPIO_PULLUP_ONLY)) != ESP_OK) return err;
    gpio_hold_dis(GR_LED);
    if ((err = gpio_set_direction(GR_LED, GPIO_MODE_OUTPUT)) != ESP_OK) return err;
    gpio_set_level(GR_LED, 0);
    /* XIAO's onboard user LED is active LOW; keep it off as well. */
    gpio_hold_dis(15);
    gpio_set_direction(15, GPIO_MODE_OUTPUT);
    gpio_set_level(15, 1);
    if (!valid_rtc) {
        gr_state_t stored;
        size_t len = sizeof(stored);
        err = nvs_get_blob(state_nvs, "checkpoint", &stored, &len);
        if (err == ESP_OK && (len != sizeof(stored) || !gr_valid(&stored))) return ESP_ERR_INVALID_CRC;
        if (err == ESP_ERR_NVS_NOT_FOUND) {
            uint8_t random[16]; char instance[33];
            esp_fill_random(random, sizeof(random));
            for (int i = 0; i < 16; i++) snprintf(instance + i * 2, 3, "%02x", random[i]);
            gr_state_init(&gr_state, instance, gpio_get_level(GR_REED), gr_now_us());
            if ((err = write_state(&gr_state)) != ESP_OK) return err;
        } else if (err == ESP_OK) gr_state = stored;
        else return err;
        /* Uncontrolled reset has an unknown reed history; baseline it without a phantom pulse. */
        gr_state.reed_level = gpio_get_level(GR_REED);
        gr_state.button_consumed = 0;
        gr_state.next_sync_us = gr_now_us() + GR_INTERVAL_US;
    }
    gr_debounce_init(&reed, gr_state.reed_level, gpio_get_level(GR_REED), esp_timer_get_time());
    if ((err = gpio_install_isr_service(ESP_INTR_FLAG_IRAM)) != ESP_OK) return err;
    if ((err = gpio_set_intr_type(GR_REED, GPIO_INTR_ANYEDGE)) != ESP_OK) return err;
    if ((err = gpio_isr_handler_add(GR_REED, reed_isr, NULL)) != ESP_OK) return err;
    if (xTaskCreate(count_task, "reed", 4096, NULL, 10, &counter_task) != pdPASS) return ESP_ERR_NO_MEM;
    return valid_rtc ? ESP_OK : gr_load_configuration();
}

void gr_snapshot(gr_state_t *s) {
    xSemaphoreTake(gr_state_mutex, portMAX_DELAY);
    drain_counter();
    *s = gr_state;
    xSemaphoreGive(gr_state_mutex);
}

esp_err_t gr_checkpoint(void) {
    xSemaphoreTake(gr_state_mutex, portMAX_DELAY);
    drain_counter();
    gr_state_t candidate = gr_state;
    esp_err_t err = write_state(&candidate);
    if (err == ESP_OK) gr_state.generation = candidate.generation;
    xSemaphoreGive(gr_state_mutex);
    return err;
}

esp_err_t gr_make_outbox(int battery_mv) {
    xSemaphoreTake(gr_state_mutex, portMAX_DELAY);
    drain_counter();
    esp_err_t err = ESP_OK;
    if (!gr_state.outbox.active) {
        gr_state_t candidate = gr_state;
        if (!gr_prepare(&candidate, battery_mv, esp_app_get_description()->version)) err = ESP_ERR_INVALID_STATE;
        else {
            err = write_state(&candidate);
            if (err == ESP_OK) gr_state = candidate;
        }
    }
    xSemaphoreGive(gr_state_mutex);
    return err;
}

esp_err_t gr_accept_ack(uint64_t sequence) {
    xSemaphoreTake(gr_state_mutex, portMAX_DELAY);
    drain_counter();
    gr_state_t candidate;
    esp_err_t err = ESP_ERR_INVALID_STATE;
    if (gr_ack_candidate(&gr_state, sequence, &candidate)) {
        err = write_state(&candidate);
        if (err == ESP_OK) gr_state = candidate;
    }
    xSemaphoreGive(gr_state_mutex);
    return err;
}

esp_err_t gr_make_reset_outbox(int battery_mv) {
    xSemaphoreTake(gr_state_mutex, portMAX_DELAY);
    drain_counter();
    gr_state_t candidate = gr_state;
    esp_err_t err = ESP_ERR_INVALID_STATE;
    if (gr_prepare_reset(&candidate, battery_mv, esp_app_get_description()->version)) {
        if (gr_state.outbox.active) err = ESP_OK;
        else {
            err = write_state(&candidate);
            if (err == ESP_OK) gr_state = candidate;
        }
    }
    xSemaphoreGive(gr_state_mutex);
    return err;
}

void gr_set_deadline(void) {
    xSemaphoreTake(gr_state_mutex, portMAX_DELAY);
    gr_state.next_sync_us = gr_now_us() + GR_INTERVAL_US;
    xSemaphoreGive(gr_state_mutex);
}

bool gr_daily_sync_due(void) {
    xSemaphoreTake(gr_state_mutex, portMAX_DELAY);
    bool due = gr_daily_due(&daily, gr_now_us());
    xSemaphoreGive(gr_state_mutex);
    return due;
}
void gr_observe_ha_time(int64_t utc_seconds) {
    /* Keep the RTC-based attempt/debounce timebase untouched by wall-clock updates. */
    xSemaphoreTake(gr_state_mutex, portMAX_DELAY);
    if (!timezone_initialized) { setenv("TZ", GR_LOCAL_TZ, 1); tzset(); timezone_initialized = true; }
    gr_daily_observe_time(&daily, gr_now_us(), utc_seconds);
    xSemaphoreGive(gr_state_mutex);
}
void gr_sync_finished(bool success) {
    xSemaphoreTake(gr_state_mutex, portMAX_DELAY);
    if (!timezone_initialized) { setenv("TZ", GR_LOCAL_TZ, 1); tzset(); timezone_initialized = true; }
    gr_daily_result(&daily, gr_now_us(), success);
    xSemaphoreGive(gr_state_mutex);
}

bool gr_counter_settled(void) {
    xSemaphoreTake(gr_state_mutex, portMAX_DELAY);
    drain_counter();
    bool settled = reed.candidate == reed.stable;
    xSemaphoreGive(gr_state_mutex);
    return settled;
}

bool gr_button_consumed(void) {
    gr_state_t s; gr_snapshot(&s); return s.button_consumed;
}

void gr_consume_button(void) {
    xSemaphoreTake(gr_state_mutex, portMAX_DELAY);
    gr_state.button_consumed = 1;
    xSemaphoreGive(gr_state_mutex);
}

void gr_led(int flashes) {
    for (int i = 0; i < flashes; i++) {
        gpio_set_level(GR_LED, 1); vTaskDelay(pdMS_TO_TICKS(80));
        gpio_set_level(GR_LED, 0); vTaskDelay(pdMS_TO_TICKS(100));
    }
}

esp_err_t gr_battery_init(void) {
    if (adc && calibration) return ESP_OK;
    adc_oneshot_unit_init_cfg_t u = {.unit_id = ADC_UNIT_1};
    esp_err_t err = adc_oneshot_new_unit(&u, &adc);
    if (err != ESP_OK) return err;
    adc_oneshot_chan_cfg_t c = {.atten = ADC_ATTEN_DB_6, .bitwidth = ADC_BITWIDTH_DEFAULT};
    if ((err = adc_oneshot_config_channel(adc, ADC_CHANNEL_0, &c)) != ESP_OK) {
        adc_oneshot_del_unit(adc); adc = NULL; return err;
    }
    adc_cali_curve_fitting_config_t fit = {.unit_id = ADC_UNIT_1, .chan = ADC_CHANNEL_0,
        .atten = ADC_ATTEN_DB_6, .bitwidth = ADC_BITWIDTH_DEFAULT};
    err = adc_cali_create_scheme_curve_fitting(&fit, &calibration);
    if (err != ESP_OK) { adc_oneshot_del_unit(adc); adc = NULL; }
    return err;
}

static int compare_int(const void *a, const void *b) { return *(const int *)a - *(const int *)b; }

esp_err_t gr_battery_read(int *mv) {
    esp_err_t err = gr_battery_init();
    if (err != ESP_OK) return err;
    int samples[16];
    for (int i = 0; i < 16; i++) {
        int raw;
        if ((err = adc_oneshot_read(adc, ADC_CHANNEL_0, &raw)) != ESP_OK) return err;
        if ((err = adc_cali_raw_to_voltage(calibration, raw, &samples[i])) != ESP_OK) return err;
        vTaskDelay(pdMS_TO_TICKS(2));
    }
    qsort(samples, 16, sizeof(int), compare_int);
    *mv = (samples[7] + samples[8]) * 1330 / (2 * 330);
    return ESP_OK;
}

void gr_sleep(void) {
    gr_network_stop();
    for (;;) {
        while (!gr_counter_settled()) vTaskDelay(pdMS_TO_TICKS(5));
        xSemaphoreTake(gr_state_mutex, portMAX_DELAY);
        drain_counter();
        if (reed.candidate != reed.stable) { xSemaphoreGive(gr_state_mutex); continue; }
        bool button_high = gpio_get_level(GR_BUTTON);
        if (!button_high && !gr_state.button_consumed) {
            /* A press racing with sleep must wake the next boot at LOW. */
        } else if (button_high) gr_state.button_consumed = 0;
        ESP_ERROR_CHECK(esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL));
        ESP_ERROR_CHECK(esp_sleep_enable_ext1_wakeup_io(1ULL << GR_REED,
            gr_state.reed_level ? ESP_EXT1_WAKEUP_ANY_LOW : ESP_EXT1_WAKEUP_ANY_HIGH));
        ESP_ERROR_CHECK(esp_sleep_enable_ext1_wakeup_io(1ULL << GR_BUTTON,
            gr_state.button_consumed ? ESP_EXT1_WAKEUP_ANY_HIGH : ESP_EXT1_WAKEUP_ANY_LOW));
        ESP_ERROR_CHECK(rtc_gpio_pullup_en(GR_BUTTON));
        ESP_ERROR_CHECK(rtc_gpio_pulldown_dis(GR_BUTTON));
        int64_t now = gr_now_us();
        int64_t regular = gr_sleep_remaining(&gr_state, now), heartbeat = gr_daily_remaining(&daily, now);
        ESP_ERROR_CHECK(esp_sleep_enable_timer_wakeup(regular < heartbeat ? regular : heartbeat));
        gpio_set_level(GR_LED, 0);
        ESP_ERROR_CHECK(gpio_hold_en(GR_LED));
        ESP_ERROR_CHECK(gpio_hold_en(15));
        /* HP interrupts cease at sleep; level wakeup captures a transition after this seal. */
        gr_seal(&gr_state);
        gr_daily_seal(&daily);
        esp_deep_sleep_start();
    }
}
