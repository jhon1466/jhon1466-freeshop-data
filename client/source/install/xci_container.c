#include "xci_container.h"

#include <string.h>
#include <stdio.h>

// Mirrors hfs0.c's on-disk header - duplicated here (rather than shared)
// because this file only needs to *validate* a candidate cheaply, without
// the full parse hfs0_parse_at does.
typedef struct {
    char magic[4];
    uint32_t num_files;
    uint32_t string_table_size;
    uint32_t reserved;
} __attribute__((packed)) Hfs0HeaderRaw;

static uint64_t read_u64_le(const uint8_t *p) {
    uint64_t v = 0;
    for (int i = 7; i >= 0; i--) v = (v << 8) | p[i];
    return v;
}

// Is there a plausible root partition table at `offset` within `buf`?
//
// "Plausible" deliberately includes finding the literal string "secure" in
// the candidate's string table: the HFS0 magic alone is only four bytes and
// does occur by chance inside compressed/encrypted content, so accepting a
// bare magic match would just move the confusing failure one step later.
// Every real XCI root table names its partitions ("secure", plus some of
// "update"/"normal"/"logo"), so requiring that is both cheap and decisive.
static bool looks_like_root_hfs0(const uint8_t *buf, size_t buf_len, uint64_t offset) {
    if (offset + sizeof(Hfs0HeaderRaw) > buf_len) return false;

    const Hfs0HeaderRaw *h = (const Hfs0HeaderRaw *)(buf + offset);
    if (memcmp(h->magic, "HFS0", 4) != 0) return false;
    // A root table holds only the handful of gamecard partitions.
    if (h->num_files == 0 || h->num_files > 8) return false;
    if (h->string_table_size == 0 || h->string_table_size > 64 * 1024) return false;

    uint64_t entries_size = (uint64_t)h->num_files * sizeof(Hfs0FileEntry);
    uint64_t string_table_offset = offset + sizeof(Hfs0HeaderRaw) + entries_size;
    if (string_table_offset + h->string_table_size > buf_len) return false;

    const char *strings = (const char *)(buf + string_table_offset);
    for (uint32_t i = 0; i + 6 <= h->string_table_size; i++) {
        if (memcmp(strings + i, "secure", 6) == 0) return true;
    }
    return false;
}

bool xci_find_root_hfs0(const uint8_t *buf, size_t buf_len, uint64_t *out_offset,
                        char *err_buf, size_t err_buf_size) {
    *out_offset = 0;

    bool has_head_magic = buf_len >= XCI_HEADER_SIZE &&
                           memcmp(buf + XCI_HEADER_MAGIC_OFFSET, "HEAD", 4) == 0;

    // 1. What the file's own header claims, when it has one.
    if (has_head_magic) {
        uint64_t stated = read_u64_le(buf + XCI_HEADER_HFS0_OFFSET_FIELD);
        if (looks_like_root_hfs0(buf, buf_len, stated)) {
            *out_offset = stated;
            return true;
        }
    }

    // 2. The two offsets real dumps actually use, in case the header's
    //    value is absent or wrong.
    static const uint64_t kWellKnown[] = { 0xF000ULL, 0x10000ULL };
    for (size_t i = 0; i < sizeof(kWellKnown) / sizeof(kWellKnown[0]); i++) {
        if (looks_like_root_hfs0(buf, buf_len, kWellKnown[i])) {
            *out_offset = kWellKnown[i];
            return true;
        }
    }

    // 3. Last resort: scan. HFS0 headers in an XCI always sit on a 0x200
    //    boundary, so stepping by that instead of byte-by-byte keeps this
    //    cheap without missing anything.
    for (uint64_t off = 0; off + sizeof(Hfs0HeaderRaw) <= buf_len; off += 0x200) {
        if (looks_like_root_hfs0(buf, buf_len, off)) {
            *out_offset = off;
            return true;
        }
    }

    if (err_buf) {
        if (!has_head_magic) {
            snprintf(err_buf, err_buf_size,
                     "el archivo no es un XCI válido (no tiene firma 'HEAD' ni una tabla de particiones "
                     "reconocible en los primeros %d KB)",
                     (int)(XCI_SEARCH_WINDOW / 1024));
        } else {
            snprintf(err_buf, err_buf_size,
                     "el archivo tiene encabezado de XCI pero no se encontró su tabla de particiones "
                     "'secure' en los primeros %d KB - puede estar incompleto o dañado",
                     (int)(XCI_SEARCH_WINDOW / 1024));
        }
    }
    return false;
}
