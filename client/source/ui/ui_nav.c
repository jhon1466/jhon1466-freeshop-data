#include "ui_nav.h"

bool nav_repeat_step(NavRepeatState *state, bool held, u64 now_tick) {
    if (!held) {
        state->was_held = false;
        return false;
    }
    if (!state->was_held) {
        state->was_held = true;
        state->next_repeat_tick = now_tick + armNsToTicks(NAV_REPEAT_DELAY_NS);
        return true;
    }
    if (now_tick >= state->next_repeat_tick) {
        state->next_repeat_tick = now_tick + armNsToTicks(NAV_REPEAT_INTERVAL_NS);
        return true;
    }
    return false;
}
