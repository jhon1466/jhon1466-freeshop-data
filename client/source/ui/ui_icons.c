#include "ui_icons.h"
#include "ui_app.h"
#include "../net/http.h"

#include <png.h>
#include <jpeglib.h>
#include <jerror.h>

#include <setjmp.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <pthread.h>

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
// guess blind.
//
// Opened once and kept open, mutex-guarded, flushed per line - NOT
// fopen/fclose per call. This log is also written from ui_list.c's own
// per-frame stall detector (see ui_icons_debug_log below), and a fopen/
// fclose pair per call is a full filesystem round trip on Switch (same
// issue torrent_log.c's own doc comment describes). A user-captured log
// proved this the hard way: once any one frame stalled past the
// detector's threshold, THIS function's own fopen/fclose cost got counted
// as part of the next frame's time, pushing it over the threshold too -
// a single real hitch turned into 1500+ consecutive logged "stalls" that
// were actually this log call re-triggering itself every frame, not a
// real ongoing stutter.
static pthread_mutex_t s_debug_log_mutex = PTHREAD_MUTEX_INITIALIZER;
static FILE *s_debug_log_fp = NULL;
static int s_debug_log_open_failed = 0;

static void icon_debug_log_write(const char *fmt, va_list ap) {
    pthread_mutex_lock(&s_debug_log_mutex);
    if (!s_debug_log_fp && !s_debug_log_open_failed) {
        s_debug_log_fp = fopen("sdmc:/switch/freeshop/icon_debug.log", "a");
        if (!s_debug_log_fp) s_debug_log_open_failed = 1;
    }
    if (s_debug_log_fp) {
        vfprintf(s_debug_log_fp, fmt, ap);
        fputc('\n', s_debug_log_fp);
        fflush(s_debug_log_fp);
    }
    pthread_mutex_unlock(&s_debug_log_mutex);
}

static void icon_debug_log(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    icon_debug_log_write(fmt, ap);
    va_end(ap);
}

void ui_icons_debug_log(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    icon_debug_log_write(fmt, ap);
    va_end(ap);
}

typedef enum {
    ICON_LOADING,   // network fetch in flight
    ICON_DECODING,  // bytes in hand (network or disk cache), queued on the decode worker
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
    // True once entry->icon_url (the primary source, e.g. the eShop CDN via
    // pipensx-metadata) has exhausted every retry and this has switched to
    // entry->icon_url_fallback instead - see ui_icons_get. Stays true even
    // if the fallback also fails, so a dead primary doesn't bounce this
    // back and forth between the two every retry window.
    bool using_fallback;
    // An on-disk-cache load has been attempted (found or not). The disk is
    // always tried before the network, but that check is itself
    // asynchronous now (see start_disk_load), so this is what stops it from
    // being re-queued every frame while it's pending or after it came back
    // empty.
    bool disk_checked;
} IconCacheEntry;

static IconCacheEntry s_cache[ICON_CACHE_MAX];
static int s_cache_count = 0;
// How many icons this pumps over the network at once. Used to be 1, then
// raised to 6 on the reasoning that a catalog can carry thousands of covers
// (see sources.h's SOURCE_KIND_TORRENT_CATALOG) and a brand new session has
// to populate the on-disk cache for all of them one request at a time
// otherwise - "this only bounds how many are outstanding together, not
// anything that blocks a frame" (each is its own thread, see
// http_get_async_start). That assumption held for socket I/O, but not for
// the CPU cost hiding behind it: mbedtls does the TLS handshake and AES in
// software here (see http.c's own comments), so 6 fetches in flight is 6
// threads of real crypto work, not 6 idle sockets. Lowering their thread
// priority (see http_get_async_start) helped but didn't fully fix it -
// icon_debug.log evidence kept showing 50-90ms frames whenever
// fetches=6/6, priority difference or not, which points at genuine
// core/memory-bus contention under that much parallel crypto rather than
// a scheduling problem alone. Down to 2: enough to not be the old fully
// serial one-at-a-time throttle, low enough that the aggregate crypto load
// stops being enough to visibly compete with the render thread. Costs
// session-start population speed for a big catalog, not steady-state
// smoothness, which is the right side of that tradeoff.
#define ICON_CONCURRENT_FETCHES 2
// Indices into s_cache currently being fetched, or -1 - see
// pump_active_fetches().
static int s_active_indices[ICON_CONCURRENT_FETCHES] = { -1, -1 };

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

// Finds a free slot in s_active_indices (a fetch just completed, or one
// was never started), or -1 if every concurrent fetch slot is busy.
static int find_free_fetch_slot(void) {
    for (int i = 0; i < ICON_CONCURRENT_FETCHES; i++) {
        if (s_active_indices[i] < 0) return i;
    }
    return -1;
}

static bool is_fetch_active(int cache_idx) {
    for (int i = 0; i < ICON_CONCURRENT_FETCHES; i++) {
        if (s_active_indices[i] == cache_idx) return true;
    }
    return false;
}

// A visible entry that finds every fetch slot busy just waits its turn
// behind whatever six entries happen to be mid-fetch - which, right after a
// scroll, can be entries that are no longer on screen at all: the grid only
// calls ui_icons_get() for what it currently draws, so an off-screen
// fetch's last_used_tick simply stops advancing the moment it scrolls away,
// while a genuinely visible one gets touched every single frame. This is
// what a user report of "icons don't load in the order things are actually
// shown on screen" traces back to - the six slots stay pinned to whatever
// started first, not to what's currently visible. Reclaiming the stalest
// slot once it's gone idle for a beat is what makes loading track the
// screen instead of scroll history. Returns -1 if every active slot was
// touched too recently to be scrolled away (more visible icons than
// concurrent slots - a real "just wait" case, not this bug).
#define ICON_STALE_TOUCH_NS 350000000ULL // ~350ms - a few frames past one scroll step
static int find_preemptable_fetch_slot(u64 now_tick) {
    int best_fetch_slot = -1;
    u64 best_age_ns = 0;
    for (int i = 0; i < ICON_CONCURRENT_FETCHES; i++) {
        int cache_idx = s_active_indices[i];
        if (cache_idx < 0) continue;
        u64 age_ns = armTicksToNs(now_tick - s_cache[cache_idx].last_used_tick);
        if (age_ns < ICON_STALE_TOUCH_NS) continue;
        if (best_fetch_slot < 0 || age_ns > best_age_ns) {
            best_fetch_slot = i;
            best_age_ns = age_ns;
        }
    }
    return best_fetch_slot;
}

// Cancels the in-flight fetch occupying `fetch_slot` (found via
// find_preemptable_fetch_slot) and frees it for a currently-visible entry.
// The preempted entry is left ICON_FAILED without bumping attempt_count, so
// if it scrolls back into view it's retried on the very next ask - see
// icon_should_retry - rather than treated as one of its real attempts.
// Safe (a no-op) to call on an already-free slot, so callers don't need to
// track whether their fetch_slot came from find_free_fetch_slot() or
// find_preemptable_fetch_slot().
static void preempt_fetch_slot(int fetch_slot) {
    int cache_idx = s_active_indices[fetch_slot];
    if (cache_idx < 0) return;
    IconCacheEntry *slot = &s_cache[cache_idx];
    if (slot->req) {
        http_async_cancel(slot->req);
        slot->req = NULL;
    }
    slot->state = ICON_FAILED;
    slot->last_attempt_tick = 0; // no cooldown - it never really failed, it just scrolled away
    icon_debug_log("[%s] preempted (scrolled off-screen) to free a fetch slot", slot->id);
    s_active_indices[fetch_slot] = -1;
}

// Starts (or restarts, on retry) a network fetch for `slot`, whose id/index
// the caller has already set up. Marks FAILED immediately if even starting
// the request doesn't work. `fetch_slot` is a free index into
// s_active_indices, from find_free_fetch_slot() - the caller checks that
// one exists before setting up `slot` at all.
static void start_fetch(IconCacheEntry *slot, int idx, int fetch_slot, const char *icon_url) {
    slot->last_attempt_tick = armGetSystemTick();
    slot->attempt_count++;
    slot->req = http_get_async_start(icon_url);
    if (!slot->req) {
        icon_debug_log("[%s] http_get_async_start failed (thread create/start refused)", slot->id);
        slot->state = ICON_FAILED;
        return;
    }
    slot->state = ICON_LOADING;
    s_active_indices[fetch_slot] = idx;
}

// Finds the cache slot least recently touched by ui_icons_get(), to reuse
// once s_cache_count has hit ICON_CACHE_MAX - excludes anything in
// s_active_indices (whatever's mid-fetch right now must not be yanked out
// from under its own HttpAsyncRequest). Always finds something:
// s_cache_count is at least 1 past ICON_CACHE_MAX to be called at all, and
// at most ICON_CONCURRENT_FETCHES of those entries are active fetches.
static int find_lru_evictable_slot(void) {
    int best = -1;
    u64 best_tick = 0;
    for (int i = 0; i < s_cache_count; i++) {
        if (is_fetch_active(i)) continue;
        // Also mid-decode (queued or on the worker right now) - evicting it
        // would let some other icon's decode result land in this slot once
        // the worker finishes (pump_decode_results only guards against a
        // *cleared* cache via s_icon_generation, not a slot recycled by
        // eviction in between).
        if (s_cache[i].state == ICON_DECODING) continue;
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

// ---- background icon worker (disk I/O + decode) ----
//
// Everything expensive about turning an icon into a texture happens here,
// on one worker thread, rather than on the render thread: reading the
// on-disk cache, writing newly-fetched bytes to it, and decoding
// (libpng/libjpeg + downscale_if_needed). The render thread only ever
// submits jobs and picks up finished pixel buffers.
//
// The decode was moved here first, on its own: it is real CPU work (a few
// ms for a typical cover, more for an oversized source), and several
// landing in one frame while scrolling a few-thousand-cover catalog (see
// sources.h's SOURCE_KIND_TORRENT_CATALOG) is a real stutter.
//
// The SD-card I/O followed for a much bigger reason, found only after
// several rounds of chasing this: the disk-cache read (read_whole_file, in
// ui_icons_get) and the post-fetch write (write_cache_file, in
// pump_one_fetch) were BOTH still running synchronously on the render
// thread. These are 200KB-1.5MB transfers to SD - tens of milliseconds
// each, every time a new cell scrolled into view or a fetch landed. That
// is the whole stall: user-captured logs showed 50-90ms frames tracking
// icon activity exactly, and neither lowering the fetch threads' priority
// nor cutting their concurrency moved it, because the cost was never on
// those threads to begin with.
//
// SDL_CreateTexture stays on the main thread (build_texture, in
// pump_decode_results below) - that part is not safe to call from a worker
// thread against a single SDL_Renderer.
//
// Deep enough that a full screen of newly-scrolled-to cells can all be
// queued in one frame rather than some being refused (which would cost
// them a wasted network round trip - see ui_icons_get's queue-full path).
#define ICON_DECODE_QUEUE_CAP 16

typedef enum {
    // `raw` already holds the bytes (a network fetch just finished). The
    // worker persists them to `path` (the disk cache) and decodes them.
    ICON_JOB_DECODE_BYTES,
    // Read `path` (the disk cache) first, then decode. `raw` is NULL.
    ICON_JOB_LOAD_FROM_DISK,
} IconJobKind;

typedef struct {
    IconJobKind kind;
    unsigned char *raw; // owned by the queue entry until the worker frees it
    size_t raw_len;
    char path[300]; // disk-cache path: read from, or write to, per `kind`
    int cache_idx;
    uint32_t generation;
} IconDecodeJob;

typedef struct {
    unsigned char *pixels; // decoded RGBA, NULL if decode failed or the source was missing
    int w, h;
    int cache_idx;
    uint32_t generation;
    // ICON_JOB_LOAD_FROM_DISK found no file. Distinct from a decode
    // failure: nothing is wrong, this icon simply isn't cached yet and
    // needs a network fetch (see pump_decode_results).
    bool source_missing;
} IconDecodeResult;

static pthread_mutex_t s_decode_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t s_decode_cond = PTHREAD_COND_INITIALIZER;
static pthread_t s_decode_thread;
static bool s_decode_thread_started = false;

static IconDecodeJob s_decode_queue[ICON_DECODE_QUEUE_CAP];
static int s_decode_queue_head = 0, s_decode_queue_count = 0;
static IconDecodeResult s_decode_results[ICON_DECODE_QUEUE_CAP];
static int s_decode_results_head = 0, s_decode_results_count = 0;

// Bumped by ui_icons_clear() so a decode that was already in flight when
// the cache was cleared gets its result discarded on pickup instead of
// being written into whatever unrelated icon now occupies that cache_idx.
// Written only from the main thread; the worker never reads it directly,
// only the per-job snapshot copied into IconDecodeJob/IconDecodeResult, so
// this needs no lock of its own.
static uint32_t s_icon_generation = 0;

static void *decode_worker_main(void *arg) {
    (void)arg;
    for (;;) {
        pthread_mutex_lock(&s_decode_mutex);
        while (s_decode_queue_count == 0)
            pthread_cond_wait(&s_decode_cond, &s_decode_mutex);
        IconDecodeJob job = s_decode_queue[s_decode_queue_head];
        s_decode_queue_head = (s_decode_queue_head + 1) % ICON_DECODE_QUEUE_CAP;
        s_decode_queue_count--;
        pthread_mutex_unlock(&s_decode_mutex);

        // All SD-card I/O for this icon happens here, on this thread - see
        // the block comment above for why that matters.
        unsigned char *raw = job.raw;
        size_t raw_len = job.raw_len;
        bool missing = false;
        if (job.kind == ICON_JOB_LOAD_FROM_DISK) {
            raw = read_whole_file(job.path, &raw_len);
            if (!raw) missing = true;
        } else if (raw && job.path[0]) {
            write_cache_file(job.path, raw, raw_len);
        }

        int w = 0, h = 0;
        unsigned char *pixels = raw ? decode_icon(raw, raw_len, &w, &h) : NULL;
        free(raw);

        pthread_mutex_lock(&s_decode_mutex);
        // Capacity matches the job queue's - a result can never be pushed
        // faster than a job was popped, so this can't overflow.
        int tail = (s_decode_results_head + s_decode_results_count) % ICON_DECODE_QUEUE_CAP;
        s_decode_results[tail].pixels = pixels;
        s_decode_results[tail].w = w;
        s_decode_results[tail].h = h;
        s_decode_results[tail].cache_idx = job.cache_idx;
        s_decode_results[tail].generation = job.generation;
        s_decode_results[tail].source_missing = missing;
        s_decode_results_count++;
        pthread_mutex_unlock(&s_decode_mutex);
    }
}

// Queues one job for the worker. For ICON_JOB_DECODE_BYTES, `raw` must be
// malloc'd and its ownership passes to the worker (freed there either way)
// - except when this returns false, which means the queue was full or the
// worker couldn't be started, and the caller still owns it. Lazily starts
// the worker thread on first use.
static bool submit_icon_job(IconJobKind kind, int cache_idx, const char *path,
                            unsigned char *raw, size_t raw_len) {
    if (!s_decode_thread_started) {
        if (pthread_create(&s_decode_thread, NULL, decode_worker_main, NULL) != 0)
            return false;
        s_decode_thread_started = true;
    }
    pthread_mutex_lock(&s_decode_mutex);
    if (s_decode_queue_count >= ICON_DECODE_QUEUE_CAP) {
        pthread_mutex_unlock(&s_decode_mutex);
        return false;
    }
    int tail = (s_decode_queue_head + s_decode_queue_count) % ICON_DECODE_QUEUE_CAP;
    s_decode_queue[tail].kind = kind;
    s_decode_queue[tail].raw = raw;
    s_decode_queue[tail].raw_len = raw_len;
    snprintf(s_decode_queue[tail].path, sizeof(s_decode_queue[tail].path), "%s", path ? path : "");
    s_decode_queue[tail].cache_idx = cache_idx;
    s_decode_queue[tail].generation = s_icon_generation;
    s_decode_queue_count++;
    pthread_cond_signal(&s_decode_cond);
    pthread_mutex_unlock(&s_decode_mutex);
    return true;
}

// Starts an on-disk-cache load for `slot` on the worker (no render-thread
// I/O). Returns true if it was queued, in which case the slot is now
// ICON_DECODING and the result arrives via pump_decode_results.
static bool start_disk_load(IconCacheEntry *slot, int idx) {
    char cache_path[300];
    snprintf(cache_path, sizeof(cache_path), "%s/%s", ICON_CACHE_DIR, slot->id);
    if (!submit_icon_job(ICON_JOB_LOAD_FROM_DISK, idx, cache_path, NULL, 0))
        return false;
    slot->disk_checked = true;
    slot->state = ICON_DECODING;
    return true;
}

// Picks up every decode the worker has finished since the last call and
// uploads it as a texture - the one part of this pipeline that has to run
// on the main thread. Cheap when there is nothing to do (one lock/unlock).
static void pump_decode_results(void) {
    for (;;) {
        pthread_mutex_lock(&s_decode_mutex);
        if (s_decode_results_count == 0) {
            pthread_mutex_unlock(&s_decode_mutex);
            return;
        }
        IconDecodeResult result = s_decode_results[s_decode_results_head];
        s_decode_results_head = (s_decode_results_head + 1) % ICON_DECODE_QUEUE_CAP;
        s_decode_results_count--;
        pthread_mutex_unlock(&s_decode_mutex);

        if (result.generation != s_icon_generation) {
            // ui_icons_clear() ran after this job was submitted - cache_idx
            // may now be a different icon entirely, or past s_cache_count.
            free(result.pixels);
            continue;
        }

        IconCacheEntry *slot = &s_cache[result.cache_idx];
        if (result.pixels) {
            SDL_Texture *tex = build_texture(result.pixels, result.w, result.h);
            slot->texture = tex;
            slot->state = tex ? ICON_READY : ICON_FAILED;
            if (!tex) icon_debug_log("[%s] decoded %dx%d but SDL_CreateTexture FAILED: %s",
                                     slot->id, result.w, result.h, SDL_GetError());
        } else if (result.source_missing) {
            // Simply not in the on-disk cache yet - nothing failed. Left
            // FAILED with no cooldown and no attempt spent, so the very
            // next ui_icons_get() starts the network fetch through the
            // ordinary retry path (see icon_should_retry: a zeroed
            // last_attempt_tick always satisfies the delay).
            slot->state = ICON_FAILED;
            slot->attempt_count = 0;
            slot->last_attempt_tick = 0;
        } else {
            // Decode itself failed - most likely a disk-cache entry that
            // got cut short on a previous write. Delete it so the next
            // attempt (the existing FAILED-state retry, which re-fetches
            // over the network - see icon_should_retry) doesn't just hit
            // the same corrupt bytes again.
            icon_debug_log("[%s] decode failed - deleting disk cache entry", slot->id);
            char cache_path[300];
            snprintf(cache_path, sizeof(cache_path), "%s/%s", ICON_CACHE_DIR, slot->id);
            remove(cache_path);
            slot->state = ICON_FAILED;
        }
    }
}

// Drives the one in-flight network fetch (if any) forward and, once it's
// done, decodes/uploads it or marks it failed. Cheap and non-blocking -
// curl_multi_perform() just checks already-readable sockets and returns;
// safe to call from ui_icons_get() itself rather than needing a dedicated
// once-per-frame hook, so callers that never call ui_icons_begin_frame()
// (ui_detail.c's single icon) still drive it correctly.
static void pump_one_fetch(int fetch_slot) {
    int cache_idx = s_active_indices[fetch_slot];
    if (cache_idx < 0) return;
    IconCacheEntry *slot = &s_cache[cache_idx];

    HttpAsyncState poll_state = http_async_poll(slot->req);
    if (poll_state == HTTP_ASYNC_RUNNING) return;

    char *net_buf = NULL;
    size_t net_len = 0;
    char err_buf[128] = {0};
    http_async_finish(slot->req, &net_buf, &net_len, err_buf, sizeof(err_buf));
    slot->req = NULL;
    s_active_indices[fetch_slot] = -1;

    slot->state = ICON_FAILED; // pessimistic default, cleared only on full success below

    if (poll_state == HTTP_ASYNC_DONE_ERROR) {
        icon_debug_log("[%s] fetch failed (attempt %d): %s", slot->id, slot->attempt_count,
                        err_buf[0] ? err_buf : "(sin mensaje)");
    }

    if (net_buf && net_len > 0) {
        char cache_path[300];
        snprintf(cache_path, sizeof(cache_path), "%s/%s", ICON_CACHE_DIR, slot->id);

        // Both the disk-cache write and the decode happen on the worker
        // (see submit_icon_job) - net_buf's ownership passes to it on
        // success. On a full queue (rare: at most ICON_CONCURRENT_FETCHES
        // fetches ever finish at once, well under the decode queue's
        // depth) this just falls back to failed-and-retried, same as any
        // other decode failure.
        if (submit_icon_job(ICON_JOB_DECODE_BYTES, cache_idx, cache_path,
                            (unsigned char *)net_buf, net_len)) {
            slot->state = ICON_DECODING;
            icon_debug_log("[%s] fetch OK, %zu bytes, queued for decode", slot->id, net_len);
            return;
        }
        icon_debug_log("[%s] fetch OK, %zu bytes, but decode queue was full", slot->id, net_len);
    } else if (poll_state == HTTP_ASYNC_DONE_OK) {
        icon_debug_log("[%s] fetch OK but empty body (0 bytes)", slot->id);
    }

    free(net_buf);
}

static void pump_active_fetches(void) {
    for (int i = 0; i < ICON_CONCURRENT_FETCHES; i++) pump_one_fetch(i);
}

void ui_icons_begin_frame(void) {
    pump_active_fetches();
    pump_decode_results();
}

SDL_Texture *ui_icons_get(const AppEntry *entry) {
    pump_active_fetches();
    pump_decode_results();

    u64 now_tick = armGetSystemTick();

    for (int i = 0; i < s_cache_count; i++) {
        if (strcmp(s_cache[i].id, entry->id) != 0) continue;

        IconCacheEntry *slot = &s_cache[i];
        slot->last_used_tick = now_tick; // being asked about at all counts as "recently used" for LRU purposes
        if (slot->state == ICON_READY) return slot->texture;

        // The on-disk cache is always tried before the network, and that
        // check is asynchronous now (see start_disk_load) - so a slot that
        // hasn't had it done yet (its first frame, or a frame where the
        // worker's queue was full) waits for that rather than going
        // straight to the network.
        if (!slot->disk_checked) {
            start_disk_load(slot, i); // stays FAILED and retries next frame if the queue is full
            return NULL;
        }

        // A failed fetch/decode is retried a few times (see
        // ICON_MAX_ATTEMPTS/ICON_RETRY_DELAY_NS) rather than staying blank
        // for the rest of the session over what's often just a momentary
        // network hiccup.
        const char *current_url = slot->using_fallback ? entry->icon_url_fallback : entry->icon_url;
        if (current_url[0] && icon_should_retry(slot, now_tick)) {
            int fetch_slot = find_free_fetch_slot();
            if (fetch_slot < 0) fetch_slot = find_preemptable_fetch_slot(now_tick);
            if (fetch_slot >= 0) {
                preempt_fetch_slot(fetch_slot); // no-op if it was already free
                icon_debug_log("[%s] retrying (attempt %d/%d)%s", slot->id, slot->attempt_count + 1,
                               ICON_MAX_ATTEMPTS, slot->using_fallback ? " [fallback]" : "");
                start_fetch(slot, i, fetch_slot, current_url);
            }
        } else if (!slot->using_fallback && slot->state == ICON_FAILED &&
                   slot->attempt_count >= ICON_MAX_ATTEMPTS && entry->icon_url_fallback[0]) {
            // The primary source (the eShop CDN, when pipensx-metadata
            // matched this entry) is exhausted - try the catalog's own
            // scraped cover before giving up on this icon for the rest of
            // the session. A real, observed failure mode: every eShop URL
            // in a session failing with "Couldn't connect to server" while
            // that scraped cover would have worked fine.
            int fetch_slot = find_free_fetch_slot();
            if (fetch_slot < 0) fetch_slot = find_preemptable_fetch_slot(now_tick);
            if (fetch_slot >= 0) {
                preempt_fetch_slot(fetch_slot);
                slot->using_fallback = true;
                slot->attempt_count = 0;
                icon_debug_log("[%s] primary source exhausted, trying fallback: %s",
                               slot->id, entry->icon_url_fallback);
                start_fetch(slot, i, fetch_slot, entry->icon_url_fallback);
            }
        }
        return NULL;
    }

    // Nothing to ever load for this entry - don't burn a cache slot on it.
    if (entry->icon_url[0] == '\0') return NULL;

    // Note there is deliberately no free-network-fetch-slot requirement
    // here any more: the first thing tried is the on-disk cache, which
    // needs no network at all. Gating slot creation on a fetch slot meant
    // that while the (now much smaller, see ICON_CONCURRENT_FETCHES) set of
    // network fetches was busy, already-cached icons elsewhere on screen
    // weren't even looked up.
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
        // Only possible if every single slot is mid-fetch or mid-decode,
        // which the queue/fetch caps make unreachable in practice - but
        // -1 would index out of bounds, so bail rather than trust that.
        if (idx < 0) return NULL;
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
    slot->using_fallback = false;
    slot->disk_checked = false;
    // FAILED-with-no-cooldown is the "needs work, nothing in flight" state
    // the loop above already knows how to pick up, so a queue-full
    // start_disk_load here simply gets retried on the next frame.
    slot->state = ICON_FAILED;

    start_disk_load(slot, idx);
    return NULL;
}

void ui_icons_clear(void) {
    for (int i = 0; i < ICON_CONCURRENT_FETCHES; i++) {
        int cache_idx = s_active_indices[i];
        if (cache_idx >= 0 && s_cache[cache_idx].req) {
            http_async_cancel(s_cache[cache_idx].req);
        }
        s_active_indices[i] = -1;
    }
    for (int i = 0; i < s_cache_count; i++) {
        if (s_cache[i].texture) SDL_DestroyTexture(s_cache[i].texture);
    }
    s_cache_count = 0;
    // Any decode still in flight for the cleared cache reports back with
    // the generation it was submitted under (see submit_decode_job) -
    // bumping this makes pump_decode_results discard that result instead
    // of writing a texture into whatever unrelated icon reuses its
    // cache_idx next.
    s_icon_generation++;
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

SDL_Texture *ui_icons_decode_bytes(const unsigned char *data, size_t size) {
    int w = 0, h = 0;
    unsigned char *pixels = decode_icon(data, size, &w, &h);
    if (!pixels) return NULL;
    return build_texture(pixels, w, h);
}

void ui_icons_debug_snapshot(char *out, size_t out_size) {
    int active_fetches = 0;
    for (int i = 0; i < ICON_CONCURRENT_FETCHES; i++) {
        if (s_active_indices[i] >= 0) active_fetches++;
    }
    int decode_q;
    pthread_mutex_lock(&s_decode_mutex);
    decode_q = s_decode_queue_count;
    pthread_mutex_unlock(&s_decode_mutex);
    snprintf(out, out_size, "cache=%d/%d fetches=%d/%d decode_q=%d/%d",
             s_cache_count, ICON_CACHE_MAX, active_fetches, ICON_CONCURRENT_FETCHES,
             decode_q, ICON_DECODE_QUEUE_CAP);
}
