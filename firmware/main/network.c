#include "runtime.h"
#include "maintenance_control.h"
#include "sync_drain.h"
#include "meter_reset.h"
#include "wifi_join.h"
#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "driver/gpio.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "mqtt_client.h"
#include "cJSON.h"
#include "mdns.h"
#include "freertos/event_groups.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#define WIFI_UP BIT0
#define WIFI_FAIL BIT1
#define MQTT_SUB BIT2
#define ACK_ERROR BIT3
#define WIFI_STOPPED BIT4
static EventGroupHandle_t events;
static QueueHandle_t acknowledgements;
static esp_mqtt_client_handle_t mqtt;
static bool initialized, wifi_started, mdns_started;
static int subscription_id;
static char report_topic[80], ack_topic[80], status_topic[80];
static char ack_buffer[1024];
static int ack_length;
int gr_portal_battery_mv;

static bool parse_u64(const char *s, uint64_t *value) {
    if (!s || !*s || strlen(s) > 20 || (s[0] == '0' && s[1])) return false;
    for (const char *p = s; *p; p++) if (*p < '0' || *p > '9') return false;
    errno = 0; char *end;
    unsigned long long result = strtoull(s, &end, 10);
    if (errno || *end) return false;
    *value = result; return true;
}

static void wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data) {
    (void)arg; (void)data;
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        if (esp_wifi_connect() != ESP_OK) xEventGroupSetBits(events, WIFI_FAIL);
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        xEventGroupClearBits(events, WIFI_UP);
        xEventGroupSetBits(events, WIFI_FAIL);
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_STOP) {
        xEventGroupClearBits(events, WIFI_UP);
        xEventGroupSetBits(events, WIFI_STOPPED);
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        xEventGroupClearBits(events, WIFI_FAIL);
        xEventGroupSetBits(events, WIFI_UP);
    }
}

static int64_t wifi_now(void *context) { (void)context; return gr_now_us(); }

static int wifi_start_attempt(void *context, bool fallback) {
    (void)context;
    wifi_config_t wc = {0};
    const char *ssid = fallback ? gr_config.fallback_ssid : gr_config.ssid;
    const char *password = fallback ? gr_config.fallback_wifi_password : gr_config.wifi_password;
    memcpy(wc.sta.ssid, ssid, strlen(ssid));
    memcpy(wc.sta.password, password, strlen(password));
    wc.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    xEventGroupClearBits(events, WIFI_UP | WIFI_FAIL | WIFI_STOPPED);
    esp_err_t err = esp_wifi_set_mode(WIFI_MODE_STA);
    if (err == ESP_OK) err = esp_wifi_set_config(WIFI_IF_STA, &wc);
    if (err == ESP_OK) err = esp_wifi_start();
    if (err == ESP_OK) wifi_started = true;
    return err;
}

static int wifi_wait_attempt(void *context, int64_t deadline) {
    (void)context;
    while (gr_now_us() < deadline) {
        EventBits_t bits = xEventGroupWaitBits(events, WIFI_UP | WIFI_FAIL, pdFALSE, pdFALSE, pdMS_TO_TICKS(10));
        if (bits & WIFI_UP) return ESP_OK;
        if (bits & WIFI_FAIL) return ESP_FAIL;
    }
    return ESP_ERR_TIMEOUT;
}

static int wifi_stop_attempt(void *context, int64_t deadline) {
    (void)context;
    if (!wifi_started) return ESP_OK;
    xEventGroupClearBits(events, WIFI_STOPPED);
    esp_err_t err = esp_wifi_stop();
    if (err != ESP_OK) return err;
    /* Wait for STOP so a delayed DISCONNECTED from the old SSID cannot fail the new attempt. */
    int64_t stop_deadline = gr_now_us() + 1000000;
    if (stop_deadline > deadline) stop_deadline = deadline;
    while (gr_now_us() < stop_deadline) {
        if (xEventGroupWaitBits(events, WIFI_STOPPED, pdFALSE, pdFALSE, pdMS_TO_TICKS(10)) & WIFI_STOPPED) {
            wifi_started = false;
            return ESP_OK;
        }
    }
    return ESP_ERR_TIMEOUT;
}

static void process_ack(void) {
    cJSON *root = cJSON_Parse(ack_buffer);
    if (!root) return;
    cJSON *instance = cJSON_GetObjectItemCaseSensitive(root, "instance_id");
    cJSON *seq = cJSON_GetObjectItemCaseSensitive(root, "sequence");
    cJSON *result = cJSON_GetObjectItemCaseSensitive(root, "result");
    cJSON *protocol = cJSON_GetObjectItemCaseSensitive(root, "protocol");
    uint64_t number;
    gr_state_t s; gr_snapshot(&s);
    if (cJSON_IsString(instance) && !strcmp(instance->valuestring, s.instance) &&
        cJSON_IsString(seq) && parse_u64(seq->valuestring, &number) &&
        cJSON_IsNumber(protocol) && protocol->valuedouble == 1 &&
        cJSON_IsString(result) && s.outbox.active && number == s.outbox.sequence) {
        if (!strcmp(result->valuestring, "ok") || !strcmp(result->valuestring, "duplicate")) {
            cJSON *utc = cJSON_GetObjectItemCaseSensitive(root, "utc_time");
            if (cJSON_IsNumber(utc) && utc->valuedouble >= GR_TIME_MIN && utc->valuedouble < GR_TIME_MAX &&
                utc->valuedouble == (int64_t)utc->valuedouble) gr_observe_ha_time((int64_t)utc->valuedouble);
            xQueueSend(acknowledgements, &number, 0);
        } else xEventGroupSetBits(events, ACK_ERROR);
    }
    cJSON_Delete(root);
}

static void mqtt_event(void *arg, esp_event_base_t base, int32_t id, void *data) {
    (void)arg; (void)base;
    esp_mqtt_event_handle_t e = data;
    if (id == MQTT_EVENT_CONNECTED) {
        xEventGroupClearBits(events, MQTT_SUB);
        subscription_id = esp_mqtt_client_subscribe(mqtt, ack_topic, 1);
    } else if (id == MQTT_EVENT_DISCONNECTED) xEventGroupClearBits(events, MQTT_SUB);
    else if (id == MQTT_EVENT_SUBSCRIBED && e->msg_id == subscription_id) xEventGroupSetBits(events, MQTT_SUB);
    else if (id == MQTT_EVENT_DATA) {
        if (e->retain) return;
        if (e->current_data_offset == 0) {
            ack_length = 0;
            if (e->topic_len != strlen(ack_topic) || memcmp(e->topic, ack_topic, e->topic_len) ||
                e->total_data_len <= 0 || e->total_data_len >= sizeof(ack_buffer)) return;
            ack_length = e->total_data_len;
        }
        if (!ack_length || e->current_data_offset < 0 || e->data_len < 0 ||
            e->current_data_offset + e->data_len > ack_length) { ack_length = 0; return; }
        memcpy(ack_buffer + e->current_data_offset, e->data, e->data_len);
        if (e->current_data_offset + e->data_len == ack_length) {
            ack_buffer[ack_length] = '\0'; process_ack(); ack_length = 0;
        }
    }
}

esp_err_t gr_network_start(int64_t deadline) {
    esp_err_t configuration_error = gr_load_configuration();
    if (configuration_error != ESP_OK) return configuration_error;
    if (wifi_started && (xEventGroupGetBits(events) & WIFI_UP)) return ESP_OK;
    if (wifi_started) {
        esp_err_t err = wifi_stop_attempt(NULL, deadline);
        if (err != ESP_OK) return err;
    }
    if (!initialized) {
        events = xEventGroupCreate(); acknowledgements = xQueueCreate(4, sizeof(uint64_t));
        if (!events || !acknowledgements) return ESP_ERR_NO_MEM;
        esp_err_t err = esp_netif_init();
        if (err != ESP_OK) return err;
        if ((err = esp_event_loop_create_default()) != ESP_OK) return err;
        if (!esp_netif_create_default_wifi_sta()) return ESP_FAIL;
        wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
        if ((err = esp_wifi_init(&cfg)) != ESP_OK) return err;
        if ((err = esp_wifi_set_storage(WIFI_STORAGE_RAM)) != ESP_OK) return err;
        if ((err = esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event, NULL)) != ESP_OK) return err;
        if ((err = esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event, NULL)) != ESP_OK) return err;
        snprintf(report_topic, sizeof(report_topic), "gas_reader/%s/report", gr_device_id);
        snprintf(ack_topic, sizeof(ack_topic), "gas_reader/%s/ack", gr_device_id);
        snprintf(status_topic, sizeof(status_topic), "gas_reader/%s/status", gr_device_id);
        initialized = true;
    }
    xEventGroupClearBits(events, WIFI_UP | WIFI_FAIL | MQTT_SUB | ACK_ERROR);
    gpio_set_direction(3, GPIO_MODE_OUTPUT); gpio_set_level(3, 0);
    vTaskDelay(pdMS_TO_TICKS(100));
    gpio_set_direction(14, GPIO_MODE_OUTPUT); gpio_set_level(14, 1);
    gr_wifi_join_ops_t join = {.now = wifi_now, .start = wifi_start_attempt,
        .wait = wifi_wait_attempt, .stop = wifi_stop_attempt, .timeout_error = ESP_ERR_TIMEOUT};
    esp_err_t err = gr_wifi_join(&join, gr_config.fallback_ssid[0] != '\0', deadline);
    if (err != ESP_OK) return err;
    if (gr_now_us() >= deadline) return ESP_ERR_TIMEOUT;
    esp_mqtt_client_config_t mc = {
        .broker.address.uri = gr_config.mqtt_uri,
        .credentials.username = gr_config.mqtt_user,
        .credentials.authentication.password = gr_config.mqtt_password,
        .session.keepalive = 15,
        .network.timeout_ms = 2000,
    };
    mqtt = esp_mqtt_client_init(&mc);
    if (!mqtt) return ESP_ERR_NO_MEM;
    if ((err = esp_mqtt_client_register_event(mqtt, ESP_EVENT_ANY_ID, mqtt_event, NULL)) != ESP_OK) return err;
    return esp_mqtt_client_start(mqtt);
}

void gr_status_publish(const char *mode) {
    if (!mqtt || !(xEventGroupGetBits(events) & MQTT_SUB)) return;
    gr_state_t s; gr_snapshot(&s);
    char data[256];
    snprintf(data, sizeof(data), "{\"protocol\":1,\"instance_id\":\"%s\",\"mode\":\"%s\",\"counter_fault\":%s}",
             s.instance, mode, s.counter_fault ? "true" : "false");
    esp_mqtt_client_enqueue(mqtt, status_topic, data, 0, 1, 0, true);
}

void gr_network_stop(void) {
    if (mdns_started) { mdns_free(); mdns_started = false; }
    if (mqtt) { esp_mqtt_client_stop(mqtt); esp_mqtt_client_destroy(mqtt); mqtt = NULL; }
    if (wifi_started) wifi_stop_attempt(NULL, gr_now_us() + 1000000);
}

static int prepare_report(void *context, int battery_mv) {
    (void)context;
    return gr_make_outbox(battery_mv);
}

static int connect_report(void *context, int64_t deadline) {
    (void)context;
    esp_err_t err;
    if ((err = gr_network_start(deadline)) != ESP_OK) return err;
    while (gr_now_us() < deadline && !(xEventGroupGetBits(events) & MQTT_SUB)) vTaskDelay(pdMS_TO_TICKS(10));
    return (xEventGroupGetBits(events) & MQTT_SUB) ? ESP_OK : ESP_ERR_TIMEOUT;
}

static int exchange_report(void *context, int64_t deadline) {
    (void)context;
    while (gr_now_us() < deadline && !(xEventGroupGetBits(events) & MQTT_SUB)) vTaskDelay(pdMS_TO_TICKS(10));
    if (gr_now_us() >= deadline || !(xEventGroupGetBits(events) & MQTT_SUB)) return ESP_ERR_TIMEOUT;
    gr_state_t s; gr_snapshot(&s);
    if (!s.outbox.active) return ESP_ERR_INVALID_STATE;
    xQueueReset(acknowledgements);
    xEventGroupClearBits(events, ACK_ERROR);
    cJSON *root = cJSON_CreateObject();
    if (!root) return ESP_ERR_NO_MEM;
    char number[21];
    cJSON_AddNumberToObject(root, "protocol", 1);
    cJSON_AddStringToObject(root, "instance_id", s.instance);
    snprintf(number, sizeof(number), "%" PRIu64, s.outbox.sequence);
    cJSON_AddStringToObject(root, "sequence", number);
    snprintf(number, sizeof(number), "%" PRIu64, s.outbox.pulses);
    cJSON_AddStringToObject(root, "pulses", number);
    cJSON_AddNumberToObject(root, "battery_mv", s.outbox.battery_mv);
    cJSON_AddStringToObject(root, "firmware", s.outbox.firmware);
    if (gr_outbox_is_reset(&s.outbox) && !cJSON_AddStringToObject(root, "operation",
        s.outbox.reserved[0] == GR_OUTBOX_LEGACY_RESET ? "reset" : "discard")) {
        /* Preserve old packet fingerprints; new discard packets are rejected
         * by old HA versions instead of invoking their destructive reset. */
        cJSON_Delete(root); return ESP_ERR_NO_MEM;
    }
    char *payload = cJSON_PrintUnformatted(root); cJSON_Delete(root);
    if (!payload) return ESP_ERR_NO_MEM;
    int msg_id = esp_mqtt_client_enqueue(mqtt, report_topic, payload, 0, 1, 0, true);
    free(payload);
    if (msg_id < 0) return ESP_FAIL;
    while (gr_now_us() < deadline) {
        uint64_t seq;
        if (xQueueReceive(acknowledgements, &seq, pdMS_TO_TICKS(20))) {
            /* A delayed duplicate ACK for the preceding packet cannot stop a
             * later packet's exchange or subtract its pulses. */
            if (seq != s.outbox.sequence) continue;
            esp_err_t err = gr_accept_ack(seq);
            return err;
        }
        if (xEventGroupGetBits(events) & ACK_ERROR) return ESP_FAIL;
    }
    return ESP_ERR_TIMEOUT;
}

static bool report_pending(void *context) {
    (void)context;
    gr_state_t s; gr_snapshot(&s);
    return s.pending != 0;
}

static esp_err_t sync_report_attempt(void) {
    int64_t deadline = gr_now_us() + 30000000;
    int mv;
    esp_err_t err = gr_battery_read(&mv);
    if (err != ESP_OK) return err;
    gr_portal_battery_mv = mv;
    gr_state_t s; gr_snapshot(&s);
    gr_sync_drain_ops_t drain = {.now = wifi_now, .prepare = prepare_report,
        .connect = connect_report, .exchange = exchange_report, .pending = report_pending,
        .refresh_saved_packet = gr_daily_sync_due() && s.outbox.active,
        .timeout_error = ESP_ERR_TIMEOUT};
    return gr_sync_drain(&drain, mv, deadline);
}

static esp_err_t sync_report(void) {
    esp_err_t err = sync_report_attempt();
    gr_sync_finished(err == ESP_OK);
    return err;
}

esp_err_t gr_sync(void) {
    gpio_set_level(GR_LED, 1);
    esp_err_t err = sync_report();
    gpio_set_level(GR_LED, 0);
    return err;
}

static bool outbox_active(void *context) {
    (void)context; gr_state_t s; gr_snapshot(&s); return s.outbox.active;
}
static bool reset_pending(void *context) {
    (void)context; gr_state_t s; gr_snapshot(&s);
    return gr_outbox_is_reset(&s.outbox);
}
static int prepare_reset_report(void *context, int battery_mv) {
    (void)context; return gr_make_reset_outbox(battery_mv);
}
esp_err_t gr_reset_meter(void) {
    int64_t deadline = gr_now_us() + 30000000;
    int mv;
    esp_err_t err = gr_battery_read(&mv);
    if (err != ESP_OK) return err;
    gr_portal_battery_mv = mv;
    gr_meter_reset_ops_t reset = {
        .sync = {.now=wifi_now,.connect=connect_report,.exchange=exchange_report,.timeout_error=ESP_ERR_TIMEOUT},
        .active=outbox_active,.reset_pending=reset_pending,.prepare_reset=prepare_reset_report
    };
    return gr_meter_reset_sync(&reset, mv, deadline);
}

static void maintenance_led_tick(void *arg) {
    bool *high = arg;
    *high = !*high;
    gpio_set_level(GR_LED, *high);
}

static void maintenance_led_stop(esp_timer_handle_t timer, bool ready) {
    /* Wait for the callback before releasing its stack argument or setting
     * the final level, so a late tick cannot overwrite the ready/error light. */
    ESP_ERROR_CHECK(esp_timer_stop_blocking(timer, portMAX_DELAY));
    ESP_ERROR_CHECK(esp_timer_delete(timer));
    gpio_set_level(GR_LED, ready);
}

esp_err_t gr_maintenance(void) {
    bool blink_high = true;
    esp_timer_handle_t blink_timer;
    esp_timer_create_args_t blink = {.callback = maintenance_led_tick, .arg = &blink_high,
        .name = "maintenance_led", .skip_unhandled_events = true};
    esp_err_t err = esp_timer_create(&blink, &blink_timer);
    if (err != ESP_OK) return err;
    gpio_set_level(GR_LED, 1);
    if ((err = esp_timer_start_periodic(blink_timer, 250000)) != ESP_OK) goto startup_failed;
    if ((err = gr_checkpoint()) != ESP_OK) goto startup_failed;
    /* Sync measures battery before radio startup and leaves the network up for the portal. */
    sync_report();
    if (!wifi_started || !(xEventGroupGetBits(events) & WIFI_UP)) {
        gr_network_stop();
        if ((err = gr_network_start(gr_now_us() + 30000000)) != ESP_OK) goto startup_failed;
    }
    if (mdns_init() == ESP_OK) {
        mdns_started = true;
        char hostname[40]; snprintf(hostname, sizeof(hostname), "gas-reader-%s", gr_device_id);
        mdns_hostname_set(hostname); mdns_instance_name_set("Gas Reader");
        mdns_service_add(NULL, "_http", "_tcp", 80, NULL, 0);
    }
    int64_t expires = gr_now_us() + 600000000;
    if ((err = gr_ota_start(expires)) != ESP_OK) goto startup_failed;
    /* Wi-Fi has an IP and all HTTP routes are registered: the portal is ready. */
    maintenance_led_stop(blink_timer, true);
    gr_status_publish("maintenance");
    gr_exit_button_t exit_button;
    gr_exit_button_init(&exit_button, gpio_get_level(GR_BUTTON), esp_timer_get_time());
    for (;;) {
        if (gr_ota_reboot_requested()) break;
        if (gr_exit_button_sample(&exit_button, gpio_get_level(GR_BUTTON), esp_timer_get_time())) {
            gr_consume_button();
            gr_ota_request_exit();
        }
        if (gr_now_us() >= expires) gr_ota_request_exit();
        if (gr_ota_can_exit()) {
            /* New uploads are blocked before saving; an existing transfer is allowed to finish. */
            gr_consume_button();
            if ((err = gr_checkpoint()) == ESP_OK) {
                break;
            }
            gr_led(2); gpio_set_level(GR_LED, 1);
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    bool reboot = gr_ota_reboot_requested();
    gr_ota_stop();
    gr_status_publish("sleeping");
    vTaskDelay(pdMS_TO_TICKS(100));
    if (reboot) {
        /* Upload handler already persisted immediately before selecting the new partition. */
        if ((err = gr_checkpoint()) != ESP_OK) return err;
        gr_network_stop();
        esp_restart();
    }
    gpio_set_level(GR_LED, 0);
    return ESP_OK;
startup_failed:
    maintenance_led_stop(blink_timer, false);
    return err;
}
