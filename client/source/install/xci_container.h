#pragma once
#include "hfs0.h"
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

// Locating an XCI's root HFS0 partition table is genuinely not a fixed
// constant, and not even reliably readable from one header field:
//
//  - A byte-for-byte physical gamecard image puts it at 0x10000, but a
//    "trimmed" dump (the norm for files shared online, cutting the
//    gamecard's wasted padding) shifts it earlier - 0xF000 is the most
//    common value.
//  - The gamecard header states its own value at XCI_HEADER_HFS0_OFFSET_FIELD,
//    which is what reference tools (hactool's `hfs0_offset`) use - but
//    real-world files exist whose header says one thing while the actual
//    HFS0 sits somewhere else entirely (observed on hardware: a header
//    claiming 0xF600 with nothing there).
//
// So: take the header's value as a hint, fall back to the well-known fixed
// offsets, and if none of those hold up, scan for the "HFS0" magic directly
// and verify each candidate actually parses as a partition table containing
// "secure". Every hardcoded-single-offset approach tried before this one
// worked for some files and failed confusingly for others.
#define XCI_HEADER_SIZE 0x200
#define XCI_HEADER_MAGIC_OFFSET 0x100      // 4 bytes, literal ASCII "HEAD"
#define XCI_HEADER_HFS0_OFFSET_FIELD 0x130 // 8 bytes, little-endian u64

// How much of the file's start to fetch/read when hunting for the root
// table - comfortably past every offset any real XCI uses, while staying
// one small bounded request over the network.
#define XCI_SEARCH_WINDOW 0x20000

// Finds the root HFS0 partition table's real offset within `buf` (which
// must be the first `buf_len` bytes of the XCI, starting at byte 0), using
// the strategy described above. Returns false with err_buf filled if
// nothing in the window parses as a root partition table.
bool xci_find_root_hfs0(const uint8_t *buf, size_t buf_len, uint64_t *out_offset,
                        char *err_buf, size_t err_buf_size);
