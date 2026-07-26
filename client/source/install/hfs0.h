#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>

// Same role as pfs0.h, but for HFS0 (the partition format XCI uses - both
// the outer/root partition table and the nested "secure" partition are
// HFS0). The only structural difference from PFS0 is a bigger file-entry
// struct (each entry also carries a hashed-region size + SHA256, used by
// other tools to verify partition integrity - unused here, matching this
// project's sha256-optional design elsewhere).
#define HFS0_MAX_ENTRIES 128
#define HFS0_MAX_NAME_LEN 64

typedef struct {
    uint64_t data_offset;
    uint64_t file_size;
    uint32_t string_table_offset;
    uint32_t hashed_size;
    uint64_t reserved;
    uint8_t hash[0x20];
} __attribute__((packed)) Hfs0FileEntry;

typedef struct {
    int count;
    Hfs0FileEntry entries[HFS0_MAX_ENTRIES];
    char names[HFS0_MAX_ENTRIES][HFS0_MAX_NAME_LEN];
    // Absolute offset within the file passed to hfs0_parse_at where this
    // partition's data region begins (right after its own header) - lets
    // hfs0_entry_file_offset() return offsets valid directly against that
    // same file, even when this Hfs0 describes a partition nested inside
    // another one (see xci_container.h).
    uint64_t data_region_offset;
} Hfs0;

// Parses the HFS0 header (base header + entry table + string table) that
// starts at `header_offset` within the already-open `fp`. Returns 0 on
// success, -1 on I/O error, -2 if it isn't a valid/supported HFS0 (bad
// magic, more entries than HFS0_MAX_ENTRIES, or a malformed string table).
int hfs0_parse_at(FILE *fp, uint64_t header_offset, Hfs0 *out);

// Same as hfs0_parse_at, but reading from `buf` (the file's first buf_len
// bytes, starting at byte 0) instead of a FILE*. Used for the root
// partition table, which callers have already fetched and validated in
// memory - re-reading those same bytes from the network just to parse them
// risks the second read disagreeing with the first, which is exactly the
// kind of failure that's near-impossible to diagnose from the outside.
int hfs0_parse_buffer(const uint8_t *buf, size_t buf_len, uint64_t header_offset, Hfs0 *out);

// Same contract as pfs0_find_by_suffix/pfs0_find_by_name.
int hfs0_find_by_suffix(const Hfs0 *p, const char *suffix, int start_from);
int hfs0_find_by_name(const Hfs0 *p, const char *name);

// Absolute byte offset of entry `index`'s data within the file hfs0_parse_at
// was called on.
uint64_t hfs0_entry_file_offset(const Hfs0 *p, int index);
