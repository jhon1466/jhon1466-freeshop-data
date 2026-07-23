#include "hfs0.h"

#include <stdlib.h>
#include <string.h>

typedef struct {
    char magic[4];
    uint32_t num_files;
    uint32_t string_table_size;
    uint32_t reserved;
} __attribute__((packed)) Hfs0Header;

int hfs0_parse_at(FILE *fp, uint64_t header_offset, Hfs0 *out) {
    memset(out, 0, sizeof(*out));

    if (fseek(fp, (long)header_offset, SEEK_SET) != 0) return -1;

    Hfs0Header header;
    if (fread(&header, sizeof(header), 1, fp) != 1) return -1;

    if (memcmp(header.magic, "HFS0", 4) != 0) return -2;
    if (header.num_files == 0 || header.num_files > HFS0_MAX_ENTRIES) return -2;

    for (uint32_t i = 0; i < header.num_files; i++) {
        if (fread(&out->entries[i], sizeof(Hfs0FileEntry), 1, fp) != 1) return -1;
    }

    if (header.string_table_size == 0 || header.string_table_size > 64 * 1024) return -2;

    char *string_table = (char *)malloc((size_t)header.string_table_size + 1);
    if (!string_table) return -1;
    if (fread(string_table, 1, header.string_table_size, fp) != header.string_table_size) {
        free(string_table);
        return -1;
    }
    string_table[header.string_table_size] = '\0';

    out->count = (int)header.num_files;
    for (int i = 0; i < out->count; i++) {
        uint32_t off = out->entries[i].string_table_offset;
        if (off >= header.string_table_size) {
            free(string_table);
            return -2;
        }
        snprintf(out->names[i], sizeof(out->names[i]), "%s", string_table + off);
    }
    free(string_table);

    out->data_region_offset = header_offset + sizeof(Hfs0Header) +
                               (uint64_t)out->count * sizeof(Hfs0FileEntry) + header.string_table_size;

    return 0;
}

static int ends_with(const char *s, const char *suffix) {
    size_t sl = strlen(s), xl = strlen(suffix);
    if (xl > sl) return 0;
    return strcmp(s + (sl - xl), suffix) == 0;
}

int hfs0_find_by_suffix(const Hfs0 *p, const char *suffix, int start_from) {
    for (int i = start_from; i < p->count; i++) {
        if (ends_with(p->names[i], suffix)) return i;
    }
    return -1;
}

int hfs0_find_by_name(const Hfs0 *p, const char *name) {
    for (int i = 0; i < p->count; i++) {
        if (strcmp(p->names[i], name) == 0) return i;
    }
    return -1;
}

uint64_t hfs0_entry_file_offset(const Hfs0 *p, int index) {
    return p->data_region_offset + p->entries[index].data_offset;
}
