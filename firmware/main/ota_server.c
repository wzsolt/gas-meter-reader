#include "runtime.h"
#include "maintenance_control.h"
#include "image_check.h"
#include <stdio.h>
#include <inttypes.h>
#include <string.h>
#include "esp_http_server.h"
#include "esp_ota_ops.h"
#include "esp_app_desc.h"
#include "esp_app_format.h"
#include "esp_timer.h"
#include "driver/gpio.h"
#include "mbedtls/base64.h"
#include "freertos/task.h"

_Static_assert(sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t) + sizeof(esp_app_desc_t)
               == GR_IMAGE_PREFIX_SIZE, "OTA parser must match ESP-IDF image layout");
static httpd_handle_t server;
static int64_t expiry;
static gr_portal_state_t portal;
static portMUX_TYPE upload_lock = portMUX_INITIALIZER_UNLOCKED;
static char expected_authorization[256];
extern const char maintenance_html[] asm("_binary_maintenance_html_start");

static bool authorized(httpd_req_t *req) {
    char header[256];
    if (httpd_req_get_hdr_value_str(req, "Authorization", header, sizeof(header)) != ESP_OK) goto deny;
    size_t len = strlen(expected_authorization);
    if (strlen(header) != len) goto deny;
    unsigned difference = 0;
    for (size_t i = 0; i < len; i++) difference |= (unsigned char)header[i] ^ (unsigned char)expected_authorization[i];
    if (!difference) return true;
deny:
    httpd_resp_set_status(req, "401 Unauthorized");
    httpd_resp_set_hdr(req, "WWW-Authenticate", "Basic realm=\"Gas Reader\"");
    httpd_resp_sendstr(req, "Authentication required");
    return false;
}

static esp_err_t page(httpd_req_t *req) {
    if (!authorized(req)) return ESP_OK;
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_sendstr(req, maintenance_html);
}

static esp_err_t status(httpd_req_t *req) {
    if (!authorized(req)) return ESP_OK;
    gr_state_t s; gr_snapshot(&s);
    /* Portal uses the last pre-radio battery snapshot rather than sampling under Wi-Fi load. */
    extern int gr_portal_battery_mv;
    char data[512];
    snprintf(data, sizeof(data), "{\"device_id\":\"%s\",\"firmware\":\"%s\",\"battery_mv\":%d,"
        "\"pending_pulses\":\"%" PRIu64 "\",\"counter_fault\":%s,\"reset_pending\":%s}", gr_device_id,
        esp_app_get_description()->version, gr_portal_battery_mv, s.pending, s.counter_fault ? "true" : "false",
        gr_outbox_is_reset(&s.outbox) ? "true" : "false");
    httpd_resp_set_type(req, "application/json"); httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_sendstr(req, data);
}

static esp_err_t fail(httpd_req_t *req, const char *status_code, const char *message) {
    httpd_resp_set_status(req, status_code);
    httpd_resp_set_hdr(req, "Connection", "close");
    httpd_resp_sendstr(req, message);
    return ESP_FAIL; /* Close socket with any unread request body. */
}

static esp_err_t exit_maintenance(httpd_req_t *req) {
    if (!authorized(req)) return ESP_FAIL;
    char guard[8];
    if (req->content_len || httpd_req_get_hdr_value_str(req, "X-Gas-Reader-Exit", guard, sizeof(guard)) != ESP_OK ||
        strcmp(guard, "1")) return fail(req, "400 Bad Request", "Exit header required; request body must be empty");
    bool waiting = gr_ota_request_exit();
    httpd_resp_set_status(req, "202 Accepted");
    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_sendstr(req, waiting ?
        "Kilépés kérve. A folyamatban lévő feltöltés után mentjük a számlálót és lezárjuk a karbantartást." :
        "Kilépés kérve. A számláló mentése után az eszköz alvó módba lép, a kapcsolat megszakad.");
}

static esp_err_t upload(httpd_req_t *req) {
    if (!authorized(req)) return ESP_FAIL;
    char guard[8], type[64];
    if (httpd_req_get_hdr_value_str(req, "X-Gas-Reader-Update", guard, sizeof(guard)) != ESP_OK || strcmp(guard, "1") ||
        httpd_req_get_hdr_value_str(req, "Content-Type", type, sizeof(type)) != ESP_OK || strcmp(type, "application/octet-stream"))
        return fail(req, "400 Bad Request", "Raw application binary and update header required");
    if (gr_now_us() >= expiry || gr_ota_reboot_requested()) return fail(req, "409 Conflict", "Maintenance window closed");
    const esp_partition_t *partition = esp_ota_get_next_update_partition(NULL);
    if (!partition || req->content_len < GR_IMAGE_PREFIX_SIZE || req->content_len > partition->size)
        return fail(req, "413 Content Too Large", "Invalid application size");
    portENTER_CRITICAL(&upload_lock);
    bool allowed = gr_portal_begin_upload(&portal);
    portEXIT_CRITICAL(&upload_lock);
    if (!allowed) return fail(req, "409 Conflict", "Maintenance is closing or an upload is already active");
    int64_t transfer_deadline = expiry + 180000000;
    uint8_t buffer[4096];
    size_t got = 0;
    esp_ota_handle_t handle = 0;
    bool began = false;
    const char *message = "Firmware transfer failed; previous application preserved";
    /* A valid descriptor must be available even when TCP splits the first header across reads. */
    while (got < GR_IMAGE_PREFIX_SIZE && gr_now_us() < transfer_deadline) {
        int n = httpd_req_recv(req, (char *)buffer + got, GR_IMAGE_PREFIX_SIZE - got);
        if (n == HTTPD_SOCK_ERR_TIMEOUT) continue;
        if (n <= 0) goto failed;
        got += n;
    }
    if (!gr_image_prefix_valid(buffer, got, req->content_len, partition->size)) {
        message = "Wrong project, target chip, or invalid image header"; goto failed;
    }
    if (gr_checkpoint() != ESP_OK) { message = "Could not checkpoint pulse counter"; goto failed; }
    /* Incremental erase limits the flash/cache-off periods while the reed ISR buffers edges. */
    if (esp_ota_begin(partition, OTA_WITH_SEQUENTIAL_WRITES, &handle) != ESP_OK) goto failed;
    began = true;
    if (esp_ota_write(handle, buffer, got) != ESP_OK) goto failed;
    size_t remaining = req->content_len - got;
    while (remaining && gr_now_us() < transfer_deadline) {
        size_t want = remaining < sizeof(buffer) ? remaining : sizeof(buffer);
        int n = httpd_req_recv(req, (char *)buffer, want);
        if (n == HTTPD_SOCK_ERR_TIMEOUT) continue;
        if (n <= 0 || esp_ota_write(handle, buffer, n) != ESP_OK) goto failed;
        remaining -= n;
    }
    if (remaining) { message = "Upload timed out"; goto failed; }
    esp_err_t err = esp_ota_end(handle); began = false;
    if (err != ESP_OK) { message = "Image validation failed"; goto failed; }
    while (!gr_counter_settled()) {
        if (gr_now_us() >= transfer_deadline) goto failed;
        vTaskDelay(pdMS_TO_TICKS(5));
    }
    if (gr_checkpoint() != ESP_OK) { message = "Could not persist counter before reboot"; goto failed; }
    if (esp_ota_set_boot_partition(partition) != ESP_OK) goto failed;
    httpd_resp_sendstr(req, "Frissítés elfogadva. Az eszköz újraindul és ellenőrzi az új firmware-t.");
    portENTER_CRITICAL(&upload_lock); gr_portal_finish_upload(&portal, true); portEXIT_CRITICAL(&upload_lock);
    return ESP_OK;
failed:
    if (began) esp_ota_abort(handle);
    portENTER_CRITICAL(&upload_lock); gr_portal_finish_upload(&portal, false); portEXIT_CRITICAL(&upload_lock);
    return fail(req, "400 Bad Request", message);
}

static esp_err_t reset_meter(httpd_req_t *req) {
    if (!authorized(req)) return ESP_FAIL;
    char guard[16];
    if (req->content_len || httpd_req_get_hdr_value_str(req, "X-Gas-Reader-Reset", guard, sizeof(guard)) != ESP_OK ||
        strcmp(guard, "confirm")) return fail(req, "400 Bad Request", "Reset confirmation header required; body must be empty");
    if (gr_now_us() >= expiry) return fail(req, "409 Conflict", "Maintenance window closed");
    portENTER_CRITICAL(&upload_lock);
    bool allowed = gr_portal_begin_reset(&portal);
    portEXIT_CRITICAL(&upload_lock);
    if (!allowed) return fail(req, "409 Conflict", "Upload, reset or maintenance shutdown in progress");
    /* Keep HTTP alive until this operation and its response have completed. */
    esp_err_t err = gr_reset_meter();
    gpio_set_level(GR_LED, 0); vTaskDelay(pdMS_TO_TICKS(100));
    gr_led(err == ESP_OK ? 3 : 2); gpio_set_level(GR_LED, 1);
    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    if (err != ESP_OK) httpd_resp_set_status(req, "503 Service Unavailable");
    gr_state_t s; gr_snapshot(&s);
    const char *message = err == ESP_OK ?
        "Az ESP resetcsomagjának függő impulzusai törölve. A HA beállított óraállása megmaradt. A csomag elkészítése után érkezett új impulzusok megmaradnak." :
        (gr_outbox_is_reset(&s.outbox) ?
         "A reset mentve, de nincs végleges visszaigazolás. A következő szinkron változatlanul újraküldi. Ellenőrizd a HA-integráció frissítését és kapcsolatát." :
         "A reset nem történt meg: a korábbi csomag rendezése vagy mentése sikertelen. Előbb szinkronizálj sikeresen, majd próbáld újra.");
    esp_err_t response = httpd_resp_sendstr(req, message);
    portENTER_CRITICAL(&upload_lock); gr_portal_finish_reset(&portal); portEXIT_CRITICAL(&upload_lock);
    return response;
}

esp_err_t gr_ota_start(int64_t expires) {
    expiry = expires;
    portENTER_CRITICAL(&upload_lock); portal = (gr_portal_state_t){0}; portEXIT_CRITICAL(&upload_lock);
    char raw[160]; snprintf(raw, sizeof(raw), "admin:%s", gr_config.ota_password);
    size_t len;
    strcpy(expected_authorization, "Basic ");
    if (mbedtls_base64_encode((unsigned char *)expected_authorization + 6, sizeof(expected_authorization) - 7,
        &len, (unsigned char *)raw, strlen(raw))) return ESP_FAIL;
    expected_authorization[6 + len] = '\0'; memset(raw, 0, sizeof(raw));
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.stack_size = 8192; config.recv_wait_timeout = 2; config.send_wait_timeout = 2;
    esp_err_t err = httpd_start(&server, &config);
    if (err != ESP_OK) return err;
    httpd_uri_t root = {.uri="/", .method=HTTP_GET, .handler=page};
    httpd_uri_t api = {.uri="/api/status", .method=HTTP_GET, .handler=status};
    httpd_uri_t ota = {.uri="/api/ota", .method=HTTP_POST, .handler=upload};
    httpd_uri_t exit = {.uri="/api/exit", .method=HTTP_POST, .handler=exit_maintenance};
    httpd_uri_t reset = {.uri="/api/reset", .method=HTTP_POST, .handler=reset_meter};
    if ((err = httpd_register_uri_handler(server, &root)) != ESP_OK ||
        (err = httpd_register_uri_handler(server, &api)) != ESP_OK ||
        (err = httpd_register_uri_handler(server, &ota)) != ESP_OK ||
        (err = httpd_register_uri_handler(server, &exit)) != ESP_OK ||
        (err = httpd_register_uri_handler(server, &reset)) != ESP_OK) gr_ota_stop();
    return err;
}

void gr_ota_stop(void) { if (server) { httpd_stop(server); server = NULL; } memset(expected_authorization, 0, sizeof(expected_authorization)); }
bool gr_ota_active(void) { portENTER_CRITICAL(&upload_lock); bool v = portal.upload_active; portEXIT_CRITICAL(&upload_lock); return v; }
bool gr_ota_reboot_requested(void) { portENTER_CRITICAL(&upload_lock); bool v = portal.reboot_requested; portEXIT_CRITICAL(&upload_lock); return v; }
bool gr_ota_request_exit(void) {
    portENTER_CRITICAL(&upload_lock);
    gr_portal_request_exit(&portal);
    bool waiting = portal.upload_active;
    portEXIT_CRITICAL(&upload_lock);
    return waiting;
}
bool gr_ota_can_exit(void) {
    portENTER_CRITICAL(&upload_lock); bool v = gr_portal_can_exit(&portal); portEXIT_CRITICAL(&upload_lock); return v;
}
