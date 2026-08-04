#include "pfs0.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    char magic[4];
    uint32_t num_files;
    uint32_t string_table_size;
    uint32_t reserved;
} __attribute__((packed)) Pfs0Header;

int pfs0_open(const char *path, Pfs0 *out) {
    memset(out, 0, sizeof(*out));

    FILE *fp = fopen(path, "rb");
    if (!fp) return -1;

    Pfs0Header header;
    if (fread(&header, sizeof(header), 1, fp) != 1) {
        fclose(fp);
        return -1;
    }

    if (memcmp(header.magic, "PFS0", 4) != 0) {
        fclose(fp);
        return -2;
    }
    if (header.num_files == 0 || header.num_files > PFS0_MAX_ENTRIES) {
        fclose(fp);
        return -2;
    }

    for (uint32_t i = 0; i < header.num_files; i++) {
        if (fread(&out->entries[i], sizeof(Pfs0FileEntry), 1, fp) != 1) {
            fclose(fp);
            return -1;
        }
    }

    if (header.string_table_size == 0 || header.string_table_size > 64 * 1024) {
        fclose(fp);
        return -2;
    }

    char *string_table = (char *)malloc((size_t)header.string_table_size + 1);
    if (!string_table) {
        fclose(fp);
        return -1;
    }
    if (fread(string_table, 1, header.string_table_size, fp) != header.string_table_size) {
        free(string_table);
        fclose(fp);
        return -1;
    }
    string_table[header.string_table_size] = '\0';

    out->count = (int)header.num_files;
    for (int i = 0; i < out->count; i++) {
        uint32_t off = out->entries[i].string_table_offset;
        if (off >= header.string_table_size) {
            free(string_table);
            fclose(fp);
            return -2;
        }
        snprintf(out->names[i], sizeof(out->names[i]), "%s", string_table + off);
    }
    free(string_table);

    out->data_region_offset = sizeof(Pfs0Header) + (uint64_t)out->count * sizeof(Pfs0FileEntry) +
                               header.string_table_size;

    fclose(fp);
    return 0;
}

int pfs0_parse_buffer(const uint8_t *buf, size_t len, Pfs0 *out) {
    memset(out, 0, sizeof(*out));

    if (len < sizeof(Pfs0Header)) return -3;

    Pfs0Header header;
    memcpy(&header, buf, sizeof(header));

    if (memcmp(header.magic, "PFS0", 4) != 0) return -2;
    if (header.num_files == 0 || header.num_files > PFS0_MAX_ENTRIES) return -2;
    if (header.string_table_size == 0 || header.string_table_size > 64 * 1024) return -2;

    size_t entries_size = (size_t)header.num_files * sizeof(Pfs0FileEntry);
    size_t header_total = sizeof(Pfs0Header) + entries_size + header.string_table_size;
    if (len < header_total) return -3;

    memcpy(out->entries, buf + sizeof(Pfs0Header), entries_size);

    const char *string_table = (const char *)(buf + sizeof(Pfs0Header) + entries_size);
    out->count = (int)header.num_files;
    for (int i = 0; i < out->count; i++) {
        uint32_t off = out->entries[i].string_table_offset;
        if (off >= header.string_table_size) return -2;
        // Bounded by the string table's own size rather than trusting it to
        // be NUL-terminated - unlike pfs0_open, which reads the table into a
        // buffer it allocates one byte larger specifically to guarantee that.
        size_t avail = (size_t)header.string_table_size - off;
        size_t name_len = strnlen(string_table + off, avail);
        if (name_len >= sizeof(out->names[i])) name_len = sizeof(out->names[i]) - 1;
        memcpy(out->names[i], string_table + off, name_len);
        out->names[i][name_len] = '\0';
    }

    out->data_region_offset = header_total;
    return 0;
}

static int ends_with(const char *s, const char *suffix) {
    size_t sl = strlen(s), xl = strlen(suffix);
    if (xl > sl) return 0;
    return strcmp(s + (sl - xl), suffix) == 0;
}

int pfs0_find_by_suffix(const Pfs0 *p, const char *suffix, int start_from) {
    for (int i = start_from; i < p->count; i++) {
        if (ends_with(p->names[i], suffix)) return i;
    }
    return -1;
}

uint64_t pfs0_entry_file_offset(const Pfs0 *p, int index) {
    return p->data_region_offset + p->entries[index].data_offset;
}
