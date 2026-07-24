#include "ui_icons.h"
#include "ui_app.h"
#include "../net/http.h"

#include <png.h>
#include <jpeglib.h>
#include <jerror.h>

#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define ICON_CACHE_DIR "sdmc:/switch/freeshop/icon_cache"
#define ICON_CACHE_MAX 128

typedef struct {
    char id[APP_ENTRY_ID_MAX];
    SDL_Texture *texture; // NULL whenever failed is true
    bool failed;
} IconCacheEntry;

static IconCacheEntry s_cache[ICON_CACHE_MAX];
static int s_cache_count = 0;
static bool s_fetched_this_frame = false;

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

static unsigned char *decode_icon(const unsigned char *data, size_t size, int *out_w, int *out_h) {
    if (size >= 8 && data[0] == 0x89 && data[1] == 'P' && data[2] == 'N' && data[3] == 'G') {
        return decode_png(data, size, out_w, out_h);
    }
    if (size >= 2 && data[0] == 0xFF && data[1] == 0xD8) {
        return decode_jpeg(data, size, out_w, out_h);
    }
    return NULL;
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

void ui_icons_begin_frame(void) {
    s_fetched_this_frame = false;
}

SDL_Texture *ui_icons_get(const AppEntry *entry) {
    for (int i = 0; i < s_cache_count; i++) {
        if (strcmp(s_cache[i].id, entry->id) == 0) {
            return s_cache[i].failed ? NULL : s_cache[i].texture;
        }
    }

    if (s_fetched_this_frame || s_cache_count >= ICON_CACHE_MAX || entry->icon_url[0] == '\0') {
        return NULL; // try again next frame (or: cache full / no icon to fetch, give up silently)
    }
    s_fetched_this_frame = true;

    IconCacheEntry *slot = &s_cache[s_cache_count++];
    snprintf(slot->id, sizeof(slot->id), "%s", entry->id);
    slot->texture = NULL;
    slot->failed = true; // pessimistic default, cleared only on full success below

    char cache_path[300];
    snprintf(cache_path, sizeof(cache_path), "%s/%s", ICON_CACHE_DIR, entry->id);

    size_t raw_len = 0;
    unsigned char *raw = read_whole_file(cache_path, &raw_len);

    if (!raw) {
        char *net_buf = NULL;
        size_t net_len = 0;
        char err_buf[128];
        if (http_get(entry->icon_url, &net_buf, &net_len, err_buf, sizeof(err_buf)) == HTTP_OK &&
            net_len > 0) {
            raw = (unsigned char *)net_buf;
            raw_len = net_len;
            write_cache_file(cache_path, raw, raw_len);
        }
    }

    if (raw) {
        int w = 0, h = 0;
        unsigned char *pixels = decode_icon(raw, raw_len, &w, &h);
        free(raw);
        if (pixels) {
            SDL_Texture *tex = build_texture(pixels, w, h);
            if (tex) {
                slot->texture = tex;
                slot->failed = false;
            }
        }
    }

    return slot->failed ? NULL : slot->texture;
}

void ui_icons_clear(void) {
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
