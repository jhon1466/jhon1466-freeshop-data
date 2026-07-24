#include "ui_icons.h"
#include "ui_app.h"
#include "../net/http.h"

#include <png.h>
#include <jpeglib.h>
#include <jerror.h>

#include <setjmp.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define ICON_CACHE_DIR "sdmc:/switch/freeshop/icon_cache"
// With decoded icons capped at ICON_MAX_DIM x ICON_MAX_DIM (256x256 RGBA =
// 256KB each, see downscale_if_needed() below), 256 cached at once is a
// ceiling of ~64MB - reasonable, and irrelevant to a catalog's actual size
// now that the cache evicts its least-recently-used entry once full (see
// find_lru_evictable_slot()) instead of just refusing anything past this
// count: a catalog with far more icons than this just means older,
// scrolled-away ones get evicted to make room, and re-fetched from the
// on-disk cache (near-instant, no network) if scrolled back to later.
#define ICON_CACHE_MAX 256
// Decoded icons are capped to this many pixels per side (see decode_jpeg's
// DCT-scaling and downscale_if_needed() below) - matches
// docs/catalog-schema.md's own "recommend ~256x256" guidance for the
// source images, just enforced client-side too instead of trusting every
// upload to follow it.
#define ICON_MAX_DIM 256

// Diagnostic-only, same pattern as main.c's update_debug_log: icons loading
// inconsistently (which ones succeed varies run to run, per a real user
// report) wasn't reproducible from here, and two different fetch mechanisms
// both showed the same symptom - logging every fetch/decode/cache decision
// point to sdmc:/switch/freeshop/icon_debug.log instead of continuing to
// guess blind. Best-effort: a failure to open the log is silently ignored.
static void icon_debug_log(const char *fmt, ...) {
    FILE *fp = fopen("sdmc:/switch/freeshop/icon_debug.log", "a");
    if (!fp) return;
    va_list ap;
    va_start(ap, fmt);
    vfprintf(fp, fmt, ap);
    va_end(ap);
    fputc('\n', fp);
    fclose(fp);
}

typedef enum {
    ICON_LOADING,
    ICON_READY,
    ICON_FAILED,
} IconState;

typedef struct {
    char id[APP_ENTRY_ID_MAX];
    SDL_Texture *texture; // valid only once state == ICON_READY
    IconState state;
    HttpAsyncRequest *req; // non-NULL only while state == ICON_LOADING and a network fetch is in flight
    u64 last_attempt_tick;
    int attempt_count;
    u64 last_used_tick; // touched on every ui_icons_get() query - drives LRU eviction once the cache is full
} IconCacheEntry;

static IconCacheEntry s_cache[ICON_CACHE_MAX];
static int s_cache_count = 0;
// Index into s_cache of the one icon currently being fetched over the
// network, or -1 - see pump_active_fetch(). Capped at one in flight at a
// time (matching the old per-frame throttle's intent: don't open a pile of
// simultaneous connections while the user scrolls through a big grid), the
// difference now being that request doesn't block anything while it's out.
static int s_active_index = -1;

// A network hiccup (common enough on this app's software-TLS connections -
// see http.c's own comments on that) shouldn't permanently blank an icon
// for the rest of the session - retry a failed fetch a few times, spaced
// out, before actually giving up on it.
#define ICON_MAX_ATTEMPTS 4
#define ICON_RETRY_DELAY_NS 3000000000ULL // 3s

static bool icon_should_retry(const IconCacheEntry *slot, u64 now_tick) {
    if (slot->state != ICON_FAILED) return false;
    if (slot->attempt_count >= ICON_MAX_ATTEMPTS) return false;
    return armTicksToNs(now_tick - slot->last_attempt_tick) >= ICON_RETRY_DELAY_NS;
}

// Starts (or restarts, on retry) a network fetch for `slot`, whose id/index
// the caller has already set up. Marks FAILED immediately if even starting
// the request doesn't work.
static void start_fetch(IconCacheEntry *slot, int idx, const char *icon_url) {
    slot->last_attempt_tick = armGetSystemTick();
    slot->attempt_count++;
    slot->req = http_get_async_start(icon_url);
    if (!slot->req) {
        slot->state = ICON_FAILED;
        return;
    }
    slot->state = ICON_LOADING;
    s_active_index = idx;
}

// Finds the cache slot least recently touched by ui_icons_get(), to reuse
// once s_cache_count has hit ICON_CACHE_MAX - excludes s_active_index
// (whatever's mid-fetch right now must not be yanked out from under its own
// HttpAsyncRequest). Always finds something: s_cache_count is at least 1
// past ICON_CACHE_MAX to be called at all, and at most one of those entries
// is the active fetch.
static int find_lru_evictable_slot(void) {
    int best = -1;
    u64 best_tick = 0;
    for (int i = 0; i < s_cache_count; i++) {
        if (i == s_active_index) continue;
        if (best < 0 || s_cache[i].last_used_tick < best_tick) {
            best = i;
            best_tick = s_cache[i].last_used_tick;
        }
    }
    return best;
}

// ---- PNG decode (in-memory, via libpng's custom read callback) ----

typedef struct {
    const unsigned char *data;
    size_t size;
    size_t pos;
} MemReader;

static void png_mem_read(png_structp png_ptr, png_bytep out, png_size_t count) {
    MemReader *r = (MemReader *)png_get_io_ptr(png_ptr);
    if (r->pos + count > r->size) {
        png_error(png_ptr, "read past end of buffer");
        return;
    }
    memcpy(out, r->data + r->pos, count);
    r->pos += count;
}

static unsigned char *decode_png(const unsigned char *data, size_t size, int *out_w, int *out_h) {
    png_structp png = png_create_read_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);
    if (!png) return NULL;
    png_infop info = png_create_info_struct(png);
    if (!info) {
        png_destroy_read_struct(&png, NULL, NULL);
        return NULL;
    }

    unsigned char *pixels = NULL;
    png_bytep *rows = NULL;

    if (setjmp(png_jmpbuf(png))) {
        free(pixels);
        free(rows);
        png_destroy_read_struct(&png, &info, NULL);
        return NULL;
    }

    MemReader reader = { data, size, 0 };
    png_set_read_fn(png, &reader, png_mem_read);
    png_read_info(png, info);

    png_uint_32 w = 0, h = 0;
    int bit_depth = 0, color_type = 0;
    png_get_IHDR(png, info, &w, &h, &bit_depth, &color_type, NULL, NULL, NULL);

    if (color_type == PNG_COLOR_TYPE_PALETTE) png_set_palette_to_rgb(png);
    if (color_type == PNG_COLOR_TYPE_GRAY && bit_depth < 8) png_set_expand_gray_1_2_4_to_8(png);
    if (png_get_valid(png, info, PNG_INFO_tRNS)) png_set_tRNS_to_alpha(png);
    if (bit_depth == 16) png_set_strip_16(png);
    if (color_type == PNG_COLOR_TYPE_GRAY || color_type == PNG_COLOR_TYPE_GRAY_ALPHA) {
        png_set_gray_to_rgb(png);
    }
    // Only entries that don't already carry an alpha channel need padding -
    // calling this unconditionally would double up the alpha byte.
    if (color_type == PNG_COLOR_TYPE_RGB || color_type == PNG_COLOR_TYPE_GRAY ||
        color_type == PNG_COLOR_TYPE_PALETTE) {
        png_set_filler(png, 0xFF, PNG_FILLER_AFTER);
    }

    png_read_update_info(png, info);

    pixels = (unsigned char *)malloc((size_t)w * h * 4);
    rows = (png_bytep *)malloc(sizeof(png_bytep) * h);
    if (!pixels || !rows) {
        free(pixels);
        free(rows);
        png_destroy_read_struct(&png, &info, NULL);
        return NULL;
    }
    for (png_uint_32 y = 0; y < h; y++) rows[y] = pixels + (size_t)y * w * 4;

    png_read_image(png, rows);

    free(rows);
    png_destroy_read_struct(&png, &info, NULL);

    *out_w = (int)w;
    *out_h = (int)h;
    return pixels;
}

// ---- JPEG decode (in-memory, via libjpeg-turbo's jpeg_mem_src) ----

struct jpeg_error_ctx {
    struct jpeg_error_mgr pub;
    jmp_buf jmp;
};

static void jpeg_error_exit(j_common_ptr cinfo) {
    struct jpeg_error_ctx *err = (struct jpeg_error_ctx *)cinfo->err;
    longjmp(err->jmp, 1);
}

static unsigned char *decode_jpeg(const unsigned char *data, size_t size, int *out_w, int *out_h) {
    struct jpeg_decompress_struct cinfo;
    struct jpeg_error_ctx jerr;

    cinfo.err = jpeg_std_error(&jerr.pub);
    jerr.pub.error_exit = jpeg_error_exit;
    if (setjmp(jerr.jmp)) {
        jpeg_destroy_decompress(&cinfo);
        return NULL;
    }

    jpeg_create_decompress(&cinfo);
    jpeg_mem_src(&cinfo, data, (unsigned long)size);
    jpeg_read_header(&cinfo, TRUE);

    // Catalog icons are supposed to be ~256x256 (docs/catalog-schema.md's
    // own recommendation) but nothing enforces that server-side - some in
    // practice are 1000px+ per side, decoding (and then holding as an SDL
    // texture) at up to ~9MB of RGBA *each* for something displayed at
    // 168x168 on screen. With ~40+ of those cached at once this exhausted
    // memory intermittently - which one failed depended on how much was
    // already allocated at that exact moment, which is why it looked random
    // and varied run to run. libjpeg can shrink the image as part of decode
    // itself (1/2, 1/4, 1/8 DCT scaling) rather than ever allocating the
    // full-resolution buffer in the first place - pick the largest of those
    // that still leaves both dimensions at or above ICON_MAX_DIM, and let
    // downscale_if_needed() below do the exact final resize.
    cinfo.scale_num = 1;
    cinfo.scale_denom = 1;
    while (cinfo.scale_denom < 8 &&
           (long)cinfo.image_width / (cinfo.scale_denom * 2) >= ICON_MAX_DIM &&
           (long)cinfo.image_height / (cinfo.scale_denom * 2) >= ICON_MAX_DIM) {
        cinfo.scale_denom *= 2;
    }

#ifdef JCS_EXT_RGBA
    cinfo.out_color_space = JCS_EXT_RGBA;
#else
    cinfo.out_color_space = JCS_RGB;
#endif

    jpeg_start_decompress(&cinfo);

    int w = (int)cinfo.output_width;
    int h = (int)cinfo.output_height;
    unsigned char *pixels = (unsigned char *)malloc((size_t)w * h * 4);
    if (!pixels) {
        jpeg_destroy_decompress(&cinfo);
        return NULL;
    }

#ifdef JCS_EXT_RGBA
    while (cinfo.output_scanline < cinfo.output_height) {
        JSAMPROW rowptr[1] = { pixels + (size_t)cinfo.output_scanline * w * 4 };
        jpeg_read_scanlines(&cinfo, rowptr, 1);
    }
#else
    unsigned char *row_buf = (unsigned char *)malloc((size_t)w * 3);
    if (!row_buf) {
        free(pixels);
        jpeg_destroy_decompress(&cinfo);
        return NULL;
    }
    while (cinfo.output_scanline < cinfo.output_height) {
        int y = (int)cinfo.output_scanline;
        JSAMPROW rowptr[1] = { row_buf };
        jpeg_read_scanlines(&cinfo, rowptr, 1);
        unsigned char *dst = pixels + (size_t)y * w * 4;
        for (int x = 0; x < w; x++) {
            dst[x * 4 + 0] = row_buf[x * 3 + 0];
            dst[x * 4 + 1] = row_buf[x * 3 + 1];
            dst[x * 4 + 2] = row_buf[x * 3 + 2];
            dst[x * 4 + 3] = 0xFF;
        }
    }
    free(row_buf);
#endif

    jpeg_finish_decompress(&cinfo);
    jpeg_destroy_decompress(&cinfo);

    *out_w = w;
    *out_h = h;
    return pixels;
}

// ---- format sniffing + shared cache/fetch plumbing ----

// Downscales `pixels` (w x h RGBA) if either dimension exceeds ICON_MAX_DIM,
// preserving aspect ratio - nearest-neighbor (icons render at 168x168 or
// smaller either way; a fancier filter's sharpness wouldn't be visible).
// PNG has no decode-time scaling option the way libjpeg does (see
// decode_jpeg's cinfo.scale_denom), so this is what actually bounds a huge
// PNG's memory use - for JPEG it's just the final precise resize after
// libjpeg's coarser 1/2-1/4-1/8 scaling already did most of the work.
// Frees the original buffer and returns a new, smaller one; if even that
// smaller allocation fails, returns the original unchanged rather than
// losing the icon entirely over a resize that couldn't happen.
static unsigned char *downscale_if_needed(unsigned char *pixels, int *w, int *h) {
    int src_w = *w, src_h = *h;
    if (src_w <= ICON_MAX_DIM && src_h <= ICON_MAX_DIM) return pixels;

    double scale = (double)ICON_MAX_DIM / (src_w > src_h ? src_w : src_h);
    int dst_w = (int)(src_w * scale);
    int dst_h = (int)(src_h * scale);
    if (dst_w < 1) dst_w = 1;
    if (dst_h < 1) dst_h = 1;

    unsigned char *dst = (unsigned char *)malloc((size_t)dst_w * dst_h * 4);
    if (!dst) return pixels;

    for (int y = 0; y < dst_h; y++) {
        int sy = (int)((int64_t)y * src_h / dst_h);
        if (sy >= src_h) sy = src_h - 1;
        for (int x = 0; x < dst_w; x++) {
            int sx = (int)((int64_t)x * src_w / dst_w);
            if (sx >= src_w) sx = src_w - 1;
            memcpy(dst + (size_t)(y * dst_w + x) * 4, pixels + (size_t)(sy * src_w + sx) * 4, 4);
        }
    }

    free(pixels);
    *w = dst_w;
    *h = dst_h;
    return dst;
}

static unsigned char *decode_icon(const unsigned char *data, size_t size, int *out_w, int *out_h) {
    unsigned char *pixels = NULL;
    if (size >= 8 && data[0] == 0x89 && data[1] == 'P' && data[2] == 'N' && data[3] == 'G') {
        pixels = decode_png(data, size, out_w, out_h);
    } else if (size >= 2 && data[0] == 0xFF && data[1] == 0xD8) {
        pixels = decode_jpeg(data, size, out_w, out_h);
    }
    if (!pixels) return NULL;
    return downscale_if_needed(pixels, out_w, out_h);
}

static unsigned char *read_whole_file(const char *path, size_t *out_len) {
    FILE *fp = fopen(path, "rb");
    if (!fp) return NULL;
    if (fseek(fp, 0, SEEK_END) != 0) { fclose(fp); return NULL; }
    long size = ftell(fp);
    if (size <= 0) { fclose(fp); return NULL; }
    fseek(fp, 0, SEEK_SET);

    unsigned char *buf = (unsigned char *)malloc((size_t)size);
    if (!buf) { fclose(fp); return NULL; }

    size_t n = fread(buf, 1, (size_t)size, fp);
    fclose(fp);
    if (n != (size_t)size) { free(buf); return NULL; }

    *out_len = n;
    return buf;
}

static void write_cache_file(const char *path, const unsigned char *data, size_t len) {
    mkdir(ICON_CACHE_DIR, 0777); // best-effort, same pattern as sources.c
    FILE *fp = fopen(path, "wb");
    if (!fp) return;
    fwrite(data, 1, len, fp);
    fclose(fp);
}

static SDL_Texture *build_texture(unsigned char *pixels, int w, int h) {
    SDL_Texture *tex = SDL_CreateTexture(g_renderer, SDL_PIXELFORMAT_RGBA32,
                                          SDL_TEXTUREACCESS_STATIC, w, h);
    if (tex) {
        SDL_UpdateTexture(tex, NULL, pixels, w * 4);
        SDL_SetTextureBlendMode(tex, SDL_BLENDMODE_BLEND);
    }
    free(pixels);
    return tex;
}

// Drives the one in-flight network fetch (if any) forward and, once it's
// done, decodes/uploads it or marks it failed. Cheap and non-blocking -
// curl_multi_perform() just checks already-readable sockets and returns;
// safe to call from ui_icons_get() itself rather than needing a dedicated
// once-per-frame hook, so callers that never call ui_icons_begin_frame()
// (ui_detail.c's single icon) still drive it correctly.
static void pump_active_fetch(void) {
    if (s_active_index < 0) return;
    IconCacheEntry *slot = &s_cache[s_active_index];

    HttpAsyncState poll_state = http_async_poll(slot->req);
    if (poll_state == HTTP_ASYNC_RUNNING) return;

    char *net_buf = NULL;
    size_t net_len = 0;
    char err_buf[128] = {0};
    http_async_finish(slot->req, &net_buf, &net_len, err_buf, sizeof(err_buf));
    slot->req = NULL;
    s_active_index = -1;

    slot->state = ICON_FAILED; // pessimistic default, cleared only on full success below

    if (poll_state == HTTP_ASYNC_DONE_ERROR) {
        icon_debug_log("[%s] fetch failed (attempt %d): %s", slot->id, slot->attempt_count,
                        err_buf[0] ? err_buf : "(sin mensaje)");
    }

    if (net_buf && net_len > 0) {
        char cache_path[300];
        snprintf(cache_path, sizeof(cache_path), "%s/%s", ICON_CACHE_DIR, slot->id);
        write_cache_file(cache_path, (unsigned char *)net_buf, net_len);

        int w = 0, h = 0;
        unsigned char *pixels = decode_icon((unsigned char *)net_buf, net_len, &w, &h);
        if (pixels) {
            SDL_Texture *tex = build_texture(pixels, w, h);
            if (tex) {
                slot->texture = tex;
                slot->state = ICON_READY;
                icon_debug_log("[%s] fetch OK, %zu bytes, decoded %dx%d, texture ready", slot->id, net_len, w, h);
            } else {
                icon_debug_log("[%s] fetch OK, %zu bytes, decoded %dx%d, but SDL_CreateTexture FAILED: %s",
                                slot->id, net_len, w, h, SDL_GetError());
            }
        } else {
            icon_debug_log("[%s] fetch OK, %zu bytes, but decode_icon FAILED (bad magic/corrupt/unsupported format - "
                            "first bytes: %02x %02x %02x %02x)",
                            slot->id, net_len,
                            net_len > 0 ? (unsigned char)net_buf[0] : 0, net_len > 1 ? (unsigned char)net_buf[1] : 0,
                            net_len > 2 ? (unsigned char)net_buf[2] : 0, net_len > 3 ? (unsigned char)net_buf[3] : 0);
        }
    } else if (poll_state == HTTP_ASYNC_DONE_OK) {
        icon_debug_log("[%s] fetch OK but empty body (0 bytes)", slot->id);
    }

    free(net_buf);
}

void ui_icons_begin_frame(void) {
    pump_active_fetch();
}

SDL_Texture *ui_icons_get(const AppEntry *entry) {
    pump_active_fetch();

    u64 now_tick = armGetSystemTick();

    for (int i = 0; i < s_cache_count; i++) {
        if (strcmp(s_cache[i].id, entry->id) != 0) continue;

        IconCacheEntry *slot = &s_cache[i];
        slot->last_used_tick = now_tick; // being asked about at all counts as "recently used" for LRU purposes
        if (slot->state == ICON_READY) return slot->texture;

        // A failed fetch/decode is retried a few times (see
        // ICON_MAX_ATTEMPTS/ICON_RETRY_DELAY_NS) rather than staying blank
        // for the rest of the session over what's often just a momentary
        // network hiccup - only one fetch in flight at a time, same as a
        // brand new icon.
        if (s_active_index < 0 && entry->icon_url[0] && icon_should_retry(slot, now_tick)) {
            icon_debug_log("[%s] retrying (attempt %d/%d)", slot->id, slot->attempt_count + 1, ICON_MAX_ATTEMPTS);
            start_fetch(slot, i, entry->icon_url);
        }
        return NULL;
    }

    if (s_active_index >= 0 || entry->icon_url[0] == '\0') {
        return NULL; // an icon is already loading, or this entry has nothing to fetch
    }

    int idx;
    if (s_cache_count < ICON_CACHE_MAX) {
        idx = s_cache_count++;
    } else {
        // Cache full - reuse whichever cached icon hasn't been asked about
        // in the longest time instead of refusing this one outright, so a
        // catalog with far more icons than ICON_CACHE_MAX still works (just
        // with older, scrolled-away icons evicted to make room - see
        // find_lru_evictable_slot()'s doc comment).
        idx = find_lru_evictable_slot();
        if (s_cache[idx].texture) SDL_DestroyTexture(s_cache[idx].texture);
        icon_debug_log("[%s] cache full (%d), evicting [%s] to make room", entry->id, ICON_CACHE_MAX, s_cache[idx].id);
    }

    IconCacheEntry *slot = &s_cache[idx];
    snprintf(slot->id, sizeof(slot->id), "%s", entry->id);
    slot->texture = NULL;
    slot->req = NULL;
    slot->attempt_count = 0;
    slot->last_attempt_tick = 0;
    slot->last_used_tick = now_tick;

    // A local disk-cache hit is fast (no network involved) - decode it
    // straight away instead of going through the async machinery below,
    // which exists specifically to avoid blocking on slow *network* I/O.
    char cache_path[300];
    snprintf(cache_path, sizeof(cache_path), "%s/%s", ICON_CACHE_DIR, entry->id);
    size_t raw_len = 0;
    unsigned char *raw = read_whole_file(cache_path, &raw_len);
    if (raw) {
        int w = 0, h = 0;
        unsigned char *pixels = decode_icon(raw, raw_len, &w, &h);
        free(raw);
        SDL_Texture *tex = pixels ? build_texture(pixels, w, h) : NULL;
        if (tex) {
            slot->texture = tex;
            slot->state = ICON_READY;
            icon_debug_log("[%s] disk cache hit, %zu bytes, decoded %dx%d", slot->id, raw_len, w, h);
            return tex;
        }
        // The cached file itself is corrupt (e.g. a previous write got cut
        // short) - remove it and fall through to a fresh network fetch
        // instead of marking this icon permanently failed over a bad local
        // copy that a re-download would fix.
        icon_debug_log("[%s] disk cache present (%zu bytes) but decode/texture FAILED (pixels=%p) - deleting and "
                        "re-fetching",
                        slot->id, raw_len, (void *)pixels);
        remove(cache_path);
    }

    icon_debug_log("[%s] starting fetch: %s", slot->id, entry->icon_url);

    // Not cached locally (or the cache was corrupt) - kick off a
    // non-blocking network fetch; pump_active_fetch() above (called every
    // time anyone asks for an icon) picks up the result once it lands.
    start_fetch(slot, idx, entry->icon_url);
    return NULL;
}

void ui_icons_clear(void) {
    if (s_active_index >= 0 && s_cache[s_active_index].req) {
        http_async_cancel(s_cache[s_active_index].req);
    }
    s_active_index = -1;
    for (int i = 0; i < s_cache_count; i++) {
        if (s_cache[i].texture) SDL_DestroyTexture(s_cache[i].texture);
    }
    s_cache_count = 0;
}

SDL_Texture *ui_icons_load_local(const char *path) {
    size_t raw_len = 0;
    unsigned char *raw = read_whole_file(path, &raw_len);
    if (!raw) return NULL;

    int w = 0, h = 0;
    unsigned char *pixels = decode_icon(raw, raw_len, &w, &h);
    free(raw);
    if (!pixels) return NULL;

    return build_texture(pixels, w, h);
}
