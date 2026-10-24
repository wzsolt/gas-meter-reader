#include "maintenance_control.h"

void gr_exit_button_init(gr_exit_button_t *button, bool high, int64_t now_us) {
    *button = (gr_exit_button_t){.candidate_high = high, .candidate_since_us = now_us,
                               .pressed_since_us = -1};
}

bool gr_exit_button_sample(gr_exit_button_t *button, bool high, int64_t now_us) {
    if (high != button->candidate_high) {
        button->candidate_high = high;
        button->candidate_since_us = now_us;
    }
    if (now_us - button->candidate_since_us < 30000) return false;
    if (high) {
        /* A stable release is mandatory, including after entering maintenance. */
        bool exit = button->armed && button->pressed_since_us >= 0 &&
            button->candidate_since_us - button->pressed_since_us < 5000000;
        button->armed = true;
        button->pressed_since_us = -1;
        return exit;
    }
    if (!button->armed) return false;
    if (button->pressed_since_us < 0) button->pressed_since_us = button->candidate_since_us;
    return false; /* A short press acts once, on its debounced release. */
}

bool gr_portal_begin_upload(gr_portal_state_t *state) {
    if (state->upload_active || state->reset_active || state->exit_requested || state->reboot_requested) return false;
    state->upload_active = true;
    return true;
}

void gr_portal_finish_upload(gr_portal_state_t *state, bool success) {
    state->upload_active = false;
    if (success) state->reboot_requested = true;
}

void gr_portal_request_exit(gr_portal_state_t *state) { state->exit_requested = true; }
bool gr_portal_can_exit(const gr_portal_state_t *state) {
    return state->exit_requested && !state->upload_active && !state->reset_active && !state->reboot_requested;
}
bool gr_portal_begin_reset(gr_portal_state_t *state) {
    if (state->upload_active || state->reset_active || state->exit_requested || state->reboot_requested) return false;
    state->reset_active = true; return true;
}
void gr_portal_finish_reset(gr_portal_state_t *state) { state->reset_active = false; }
