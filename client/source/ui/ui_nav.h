#pragma once
#include <switch.h>
#include <stdbool.h>

// Directional navigation (D-Pad or left stick) auto-repeats while held,
// instead of requiring a fresh press per step - the classic "tap to move
// one, hold to fast-scroll" pattern. First step is immediate on press; once
// held past NAV_REPEAT_DELAY_NS it repeats every NAV_REPEAT_INTERVAL_NS.
// Shared by every screen with a directional list/grid (ui_list.c,
// ui_explorer.c).
#define NAV_REPEAT_DELAY_NS 350000000ULL
#define NAV_REPEAT_INTERVAL_NS 120000000ULL
// How far the left stick has to be pushed (of a ~32767 max) before it counts
// as "held" in that direction - well past resting/drift noise, well short
// of needing a full-deflection press.
#define NAV_STICK_DEADZONE 13000

typedef struct {
    bool was_held;
    u64 next_repeat_tick;
} NavRepeatState;

// Returns true once for a "step" in this direction: immediately on the
// press (was_held transitioning false -> true), then repeatedly while held,
// starting NAV_REPEAT_DELAY_NS after the press and every
// NAV_REPEAT_INTERVAL_NS after that. `state` is one direction's own
// persisted timing (a caller with 4 directions needs 4 of these, one each)
// - each direction repeats independently of the others.
bool nav_repeat_step(NavRepeatState *state, bool held, u64 now_tick);
