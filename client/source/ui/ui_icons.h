#pragma once
#include "../catalog/app_entry.h"
#include <SDL2/SDL.h>

// Call once per rendered frame (grid view only - the list view doesn't show
// icons) before requesting textures for that frame's visible cells. Purely
// a convenience hook to drive any in-flight icon fetch forward a step even
// on a frame that doesn't end up calling ui_icons_get() for any cell -
// ui_icons_get() itself already does this too, so skipping this call (as
// ui_detail.c does, for its one icon) is harmless.
void ui_icons_begin_frame(void);

// Returns a cached texture for entry->id (fetched from entry->icon_url on
// first request - cached in memory afterwards, and on the SD card so future
// launches skip the network fetch), or NULL if not yet available: loading
// is pending (try again next frame - the fetch itself never blocks, so the
// render loop and input keep running while it's in flight), or the icon
// failed to fetch/decode (won't be retried again this session). At most one
// icon is fetched over the network at a time; requests for other not-yet-
// cached icons just return NULL until it's their turn. Caller must not
// destroy the returned texture - ui_icons_clear() owns it.
SDL_Texture *ui_icons_get(const AppEntry *entry);

// Destroys every cached texture. Call after catalog_free() when entries
// change (ids may now refer to different apps) and once more at shutdown.
void ui_icons_clear(void);

// Decodes a local PNG/JPEG file (e.g. romfs:/qr.jpg) into a new texture -
// not cached/tracked by ui_icons_clear(), the caller owns the result and
// must SDL_DestroyTexture() it. Returns NULL on any read/decode failure.
SDL_Texture *ui_icons_load_local(const char *path);

// Same as ui_icons_load_local but decodes PNG/JPEG bytes already in memory
// (e.g. the NACP icon buffer from nsGetApplicationControlData) instead of
// reading a file - not cached/tracked by ui_icons_clear(), caller owns the
// result. Returns NULL on any decode failure.
SDL_Texture *ui_icons_decode_bytes(const unsigned char *data, size_t size);
