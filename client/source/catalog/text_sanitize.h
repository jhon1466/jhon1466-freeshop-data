#pragma once
// Small UTF-8 helper shared by catalog.c and game_metadata.c: strips
// codepoints the console's bundled/shared TTF font has no glyph for before
// any scraped/third-party text (catalog JSON, pipensx-metadata descriptions)
// reaches an AppEntry field. Without this, a description that uses emoji or
// dingbat bullets (a mountain, a game controller, a star, "*") as section
// markers - common in Nintendo eShop-style copy - renders each one as a
// "tofu" missing-glyph box on screen instead of nothing.
#include <stdint.h>
#include <string.h>

// Decodes one UTF-8 codepoint starting at s[0..remaining). Always advances
// *len by at least 1, even for an invalid/truncated sequence (returned as
// the U+FFFD replacement character), so a caller looping over a string can
// never stall or read past `remaining`.
static inline uint32_t utf8_decode_cp(const unsigned char *s, size_t remaining, size_t *len) {
    unsigned char c = s[0];
    if (c < 0x80) { *len = 1; return c; }

    int extra;
    uint32_t cp;
    if ((c & 0xE0) == 0xC0) { extra = 1; cp = c & 0x1F; }
    else if ((c & 0xF0) == 0xE0) { extra = 2; cp = c & 0x0F; }
    else if ((c & 0xF8) == 0xF0) { extra = 3; cp = c & 0x07; }
    else { *len = 1; return 0xFFFD; }

    if ((size_t)(extra + 1) > remaining) { *len = 1; return 0xFFFD; }
    for (int i = 1; i <= extra; i++) {
        unsigned char cc = s[i];
        if ((cc & 0xC0) != 0x80) { *len = 1; return 0xFFFD; }
        cp = (cp << 6) | (cc & 0x3F);
    }
    *len = (size_t)(extra + 1);
    return cp;
}

// Codepoint ranges this client has never been confirmed to render: arrows,
// geometric shapes, misc symbols, dingbats, and the emoji/supplemental
// symbol planes - exactly the blocks used for decorative bullets/section
// markers. Ordinary text - Latin, Latin-1/Extended, general punctuation
// (em/en dash, curly quotes, all below U+2190), and Cyrillic - is untouched.
static inline int utf8_cp_is_unrenderable(uint32_t cp) {
    return (cp >= 0x2190 && cp <= 0x2BFF)
        || (cp >= 0xFE00 && cp <= 0xFE0F)   // variation selectors
        || (cp >= 0x1F000 && cp <= 0x1FFFF); // emoji & supplemental symbols
}

// Copies `in` into `out` (NUL-terminated, at most out_size-1 bytes),
// dropping codepoints utf8_cp_is_unrenderable() flags. A run of dropped
// codepoints collapses to a single space rather than disappearing outright,
// so text on either side doesn't get glued together (a removed bullet
// between "GAMEPLAY" and "Platforming" would otherwise read
// "GAMEPLAYPlatforming"). Output is always <= input in byte length, since
// every dropped run is replaced by at most one 1-byte space.
static inline void utf8_strip_unrenderable(const char *in, char *out, size_t out_size) {
    if (out_size == 0) return;
    if (!in) { out[0] = '\0'; return; }

    size_t in_len = strlen(in);
    size_t oi = 0;
    int pending_space = 0;
    for (size_t ii = 0; ii < in_len && oi + 1 < out_size; ) {
        size_t clen;
        uint32_t cp = utf8_decode_cp((const unsigned char *)in + ii, in_len - ii, &clen);
        if (utf8_cp_is_unrenderable(cp)) {
            if (oi > 0) pending_space = 1; // only matters if real text follows
        } else {
            if (pending_space && out[oi - 1] != ' ') {
                if (oi + 1 >= out_size) break;
                out[oi++] = ' ';
            }
            pending_space = 0;
            if (oi + clen >= out_size) break; // no room for the whole codepoint
            memcpy(out + oi, in + ii, clen);
            oi += clen;
        }
        ii += clen;
    }
    while (oi > 0 && out[oi - 1] == ' ') oi--; // trailing run left no follower
    out[oi] = '\0';
}
