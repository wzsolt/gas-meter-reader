#pragma once
#include "core.h"
#include "daily_schedule.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#define GR_REED 1
#define GR_BUTTON 2
#define GR_LED 22

typedef struct {
    char ssid[33];
    char wifi_password[65];
    char fallback_ssid[33];
    char fallback_wifi_password[65];
    char mqtt_uri[256];
    char mqtt_user[65];
    char mqtt_password[129];
    char ota_password[129];
} gr_config_t;

extern gr_config_t gr_config;
extern char gr_device_id[13];
extern gr_state_t gr_state;
extern SemaphoreHandle_t gr_state_mutex;

int64_t gr_now_us(void);
esp_err_t gr_runtime_init(bool deep_wake);
esp_err_t gr_load_configuration(void);
esp_err_t gr_checkpoint(void);
esp_err_t gr_make_outbox(int battery_mv);
esp_err_t gr_accept_ack(uint64_t sequence);
esp_err_t gr_make_reset_outbox(int battery_mv);
void gr_snapshot(gr_state_t *s);
void gr_set_deadline(void);
bool gr_daily_sync_due(void);
void gr_observe_ha_time(int64_t utc_seconds);
void gr_sync_finished(bool success);
void gr_led(int flashes);
esp_err_t gr_battery_init(void);
esp_err_t gr_battery_read(int *mv);
bool gr_counter_settled(void);
bool gr_button_consumed(void);
void gr_consume_button(void);
void gr_sleep(void) __attribute__((noreturn));

esp_err_t gr_network_start(int64_t deadline);
void gr_network_stop(void);
esp_err_t gr_sync(void);
esp_err_t gr_reset_meter(void);
esp_err_t gr_maintenance(void);
void gr_status_publish(const char *mode);

esp_err_t gr_ota_start(int64_t expires);
void gr_ota_stop(void);
bool gr_ota_active(void);
bool gr_ota_reboot_requested(void);
bool gr_ota_request_exit(void);
bool gr_ota_can_exit(void);
