#pragma once
#include "hfs0.h"

// The root HFS0 partition table in a real Switch game card image always
// starts at this fixed offset (public, documented on switchbrew.org's XCI
// format page) - everything before it is the card header/certificate area,
// which installers never need to touch.
#define XCI_ROOT_HFS0_OFFSET 0xF000ULL

// Opens the XCI at `path`, parses its root partition table at
// XCI_ROOT_HFS0_OFFSET, finds the "secure" partition within it (the one
// holding the actual title's NCAs/cnmt/tik/cert - "update"/"normal"/"logo"
// are the other, irrelevant partitions), and parses THAT partition's own
// nested HFS0 header. `out` describes the secure partition's file entries
// with data_region_offset already resolved to an absolute offset within the
// XCI file, so hfs0_entry_file_offset(out, ...) can be used exactly like
// pfs0_entry_file_offset() is for an NSP - callers don't need to know
// anything about the nesting.
//
// Returns 0 on success, -1 on I/O error, -2 if the file isn't a valid XCI or
// has no "secure" partition.
int xci_open_secure_partition(const char *path, Hfs0 *out);
