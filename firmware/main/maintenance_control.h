#pragma once
#include <stdbool.h>
#include <stdint.h>

typedef struct {
    bool candidate_high, armed;
    int64_t candidate_since_us, pressed_since_us;
} gr_exit_button_t;

void gr_exit_button_init(gr_exit_button_t *button, bool high, int64_t now_us);
bool gr_exit_button_sample(gr_exit_button_t *button, bool high, int64_t now_us);

/* Call under the portal lock when shared between HTTP and maintenance tasks. */
typedef struct {
    bool upload_active, reboot_requested, exit_requested, reset_active;
} gr_portal_state_t;
bool gr_portal_begin_upload(gr_portal_state_t *state);
void gr_portal_finish_upload(gr_portal_state_t *state, bool success);
void gr_portal_request_exit(gr_portal_state_t *state);
bool gr_portal_can_exit(const gr_portal_state_t *state);
bool gr_portal_begin_reset(gr_portal_state_t *state);
void gr_portal_finish_reset(gr_portal_state_t *state);
