#pragma once
#include <switch.h>
#include <stdbool.h>
#include <stddef.h>

typedef struct {
    s64 sd_total, sd_free;
    s64 nand_total, nand_free;
    bool sd_ok;   // false -> render "unavailable" instead of a bar
    bool nand_ok; // false -> render "unavailable" instead of a bar
} StorageInfo;

// Display-only space query, separate from the statvfs-based pre-download
// free-space check in install/install.c. Never fails loudly - on any
// service error the corresponding *_ok flag is left false so the caller
// can render "unavailable" rather than blocking or crashing.
void ui_storage_refresh(StorageInfo *out);

void format_bytes(s64 bytes, char *out, size_t out_size);
