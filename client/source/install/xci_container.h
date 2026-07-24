#pragma once
#include "hfs0.h"

// The root HFS0 partition table in a real Switch game card image always
// starts at this fixed offset (public, documented on switchbrew.org's XCI
// format page) - everything before it is the card header/certificate area,
// which installers never need to touch.
//
// Locating the "secure" partition within that root table (the one holding
// the actual title's NCAs/cnmt/tik/cert - "update"/"normal"/"logo" are the
// other, irrelevant partitions) is done directly in install_xci_native.c's
// xci_open_secure_partition_from_url(), which needs it over the network
// (two small Range GETs: the root table here, then the secure entry's own
// nested header) rather than against a local file.
#define XCI_ROOT_HFS0_OFFSET 0xF000ULL
