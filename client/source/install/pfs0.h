#pragma once
#include <stdint.h>
#include <stddef.h>

// Generous - a typical single-game NSP has a handful of NCAs plus one
// tik/cert pair; multi-content bundles rarely exceed a few dozen entries.
#define PFS0_MAX_ENTRIES 128
#define PFS0_MAX_NAME_LEN 64

typedef struct {
    uint64_t data_offset;
    uint64_t file_size;
    uint32_t string_table_offset;
    uint32_t reserved;
} __attribute__((packed)) Pfs0FileEntry;

// A parsed PFS0 (NSP) container's directory - header/entry-table/string-table
// only. The (potentially multi-GB) file-data region is never buffered; use
// pfs0_entry_file_offset() to seek into the source file for that.
typedef struct {
    int count;
    Pfs0FileEntry entries[PFS0_MAX_ENTRIES];
    char names[PFS0_MAX_ENTRIES][PFS0_MAX_NAME_LEN];
    uint64_t data_region_offset;
} Pfs0;

// Reads and parses the PFS0 header/entry-table/string-table of the file at
// `path`. Returns 0 on success, -1 on I/O error, -2 if the file isn't a
// valid/supported PFS0 (bad magic, more entries than PFS0_MAX_ENTRIES, or a
// malformed string table).
int pfs0_open(const char *path, Pfs0 *out);

// Same parse, from an in-memory prefix of the container instead of a file -
// the whole header (magic, entry table, string table) lives at the very
// start of an NSP, so a caller receiving one as a byte stream can parse the
// directory before the file data arrives. Returns 0 on success, -2 if the
// bytes definitively aren't a valid/supported PFS0, or -3 if `len` simply
// doesn't cover the whole header yet - the caller should accumulate more
// and call again. On -3, `out` is left unusable.
int pfs0_parse_buffer(const uint8_t *buf, size_t len, Pfs0 *out);

// Index of the first entry (at or after `start_from`) whose name ends with
// `suffix` (case-sensitive, e.g. ".cnmt.nca", ".tik", ".cert"), or -1 if none
// match. Pass 0 for `start_from` to search from the beginning.
int pfs0_find_by_suffix(const Pfs0 *p, const char *suffix, int start_from);

// Absolute byte offset of entry `index`'s data within the file passed to
// pfs0_open (the data region begins right after the string table).
uint64_t pfs0_entry_file_offset(const Pfs0 *p, int index);
