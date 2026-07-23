#pragma once
#include "../catalog/app_entry.h"
#include <SDL2/SDL.h>

// Call once per rendered frame (grid view only - the list view doesn't show
// icons) before requesting textures for that frame's visible cells. Bounds
// icon loading to at most one new network fetch + decode per frame, so a
// slow connection can't stall the whole render loop - cells not yet loaded
// just get NULL back (draw a placeholder) and are retried on a later frame.
void ui_icons_begin_frame(void);

// Returns a cached texture for entry->id (decoded from entry->icon_url on
// first request - cached in memory afterwards, and on the SD card so future
// launches skip the network fetch), or NULL if not yet available: loading
// is pending (try again next frame), or the icon failed to fetch/decode
// (won't be retried again this session). Caller must not destroy the
// returned texture - ui_icons_clear() owns it.
SDL_Texture *ui_icons_get(const AppEntry *entry);

// Destroys every cached texture. Call after catalog_free() when entries
// change (ids may now refer to different apps) and once more at shutdown.
void ui_icons_clear(void);
