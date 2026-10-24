#include "runtime.h"
#include "driver/gpio.h"
#include "esp_sleep.h"
#include "esp_ota_ops.h"
#include "esp_log.h"
#include "freertos/task.h"

static void handle_result(esp_err_t err) {
    gpio_set_level(GR_LED, 0);
    vTaskDelay(pdMS_TO_TICKS(100)); /* Separate the steady busy light from all result flashes. */
    gr_led(err == ESP_OK ? 3 : 2);
}

void app_main(void) {
    /* The new API returns a bitmap; a cold boot sets the UNDEFINED bit. */
    uint32_t wakeup_causes = esp_sleep_get_wakeup_causes();
    bool deep = (wakeup_causes & ~(UINT32_C(1) << ESP_SLEEP_WAKEUP_UNDEFINED)) != 0;
    esp_err_t err = gr_runtime_init(deep);
    if (err != ESP_OK) {
        esp_ota_img_states_t state;
        if (esp_ota_get_state_partition(esp_ota_get_running_partition(), &state) == ESP_OK &&
            state == ESP_OTA_IMG_PENDING_VERIFY) esp_ota_mark_app_invalid_rollback_and_reboot();
        ESP_LOGE("gas_reader", "Startup/configuration failed: %s. NVS was preserved.", esp_err_to_name(err));
        /* No counting claims and no NVS erase on damaged state or missing provisioning. */
        gpio_set_direction(GR_LED, GPIO_MODE_OUTPUT);
        for (;;) {
            gr_led(2); vTaskDelay(pdMS_TO_TICKS(5000));
        }
    }
    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_ota_img_states_t ota_state;
    if (esp_ota_get_state_partition(running, &ota_state) == ESP_OK && ota_state == ESP_OTA_IMG_PENDING_VERIFY) {
        if (gr_battery_init() != ESP_OK || !gr_state_mutex) esp_ota_mark_app_invalid_rollback_and_reboot();
        ESP_ERROR_CHECK(esp_ota_mark_app_valid_cancel_rollback());
        gr_led(3);
    }
    if (!deep) vTaskDelay(pdMS_TO_TICKS(150));
    while (!gr_counter_settled()) vTaskDelay(pdMS_TO_TICKS(5));

    if (!gpio_get_level(GR_BUTTON) && !gr_button_consumed()) {
        int64_t start = gr_now_us();
        vTaskDelay(pdMS_TO_TICKS(30));
        if (!gpio_get_level(GR_BUTTON)) {
            while (!gpio_get_level(GR_BUTTON) && gr_now_us() - start < 5000000)
                vTaskDelay(pdMS_TO_TICKS(5));
            bool long_press = !gpio_get_level(GR_BUTTON);
            gr_consume_button();
            esp_err_t result = long_press ? gr_maintenance() : gr_sync();
            handle_result(result);
            gr_set_deadline();
        }
    }
    gr_state_t s; gr_snapshot(&s);
    if (gr_daily_sync_due() || gr_now_us() >= s.next_sync_us) {
        if (gr_daily_sync_due() || gr_automatic_sync_due(&s, gr_now_us())) handle_result(gr_sync());
        gr_set_deadline();
    }
    gr_sleep();
}
