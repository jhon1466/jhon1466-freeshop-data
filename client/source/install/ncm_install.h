#pragma once
#include <switch.h>
#include <stdio.h>
#include <stdbool.h>
#include "install.h"
#include "install_common.h"

// A single parsed CNMT (content meta): identity plus every content record it
// lists (the Meta/cnmt record itself is added back explicitly by the caller
// when committing - see ncm_commit_content_meta - since it isn't part of the
// CNMT's own content list).
#define NCM_MAX_CONTENT_INFOS 32
// Generous - Application/Patch/AddOnContent extended headers are all well
// under this (the largest, NcmPatchMetaExtendedHeader, is 0x18 bytes).
#define NCM_MAX_EXTENDED_HEADER 0x400

typedef struct {
    NcmContentMetaKey key;
    uint16_t extended_header_size;
    uint16_t content_meta_count;
    uint8_t attributes;
    uint8_t raw_extended_header[NCM_MAX_EXTENDED_HEADER];
    int content_info_count;
    NcmContentInfo content_infos[NCM_MAX_CONTENT_INFOS];
} ContentMetaInfo;

// Parses a 32-hex-character NCA id (e.g. from a PFS0 entry name like
// "0123...cdef.nca" or "0123...cdef.cnmt.nca") into an NcmContentId. Returns
// false if the leading 32 characters aren't valid hex.
bool ncm_parse_content_id(const char *name, NcmContentId *out);

// Lowercase-hex-encodes `id` into `out_hex` (32 chars + NUL) - the inverse of
// ncm_parse_content_id, used to look a CNMT-referenced content id back up by
// its ".nca" filename within the source PFS0.
void ncm_format_content_id(const NcmContentId *id, char out_hex[33]);

// Streams `size` bytes starting at `file_offset` from `src` into a freshly
// created placeholder for `content_id` in `cs`, then registers it. `cb`/
// `userdata` drive the shared install progress UI (see install.h) - returning
// false from `cb` aborts midway and the partial placeholder is deleted.
// If `content_id` is already present in `cs`, returns true immediately
// without re-writing it (base/update/DLC installs can share NCAs).
//
// `out_registered` (may be NULL) - same contract as
// ncm_install_content_from_url's: true only if this call actually
// registered new content, false if content_id was already present and
// nothing was written - callers rolling back a failed/canceled install need
// this to avoid deleting content that belongs to some other, already-
// installed title.
// `gate` (may be NULL) blocks each chunk read until those bytes have
// arrived, for a source still being written - see install_common.h's
// InstallReadGate. It closes/reopens *src, which is why this takes FILE**.
bool ncm_install_content(NcmContentStorage *cs, const NcmContentId *content_id,
                          FILE **src, uint64_t file_offset, uint64_t size,
                          const InstallReadGate *gate,
                          InstallProgressCallback cb, void *userdata,
                          bool *out_registered,
                          char *err_buf, size_t err_buf_size);

// Same contract as ncm_install_content, but sources `size` bytes starting at
// `file_offset` from an HTTP Range request against `ru` instead of a local
// FILE*, streaming each chunk directly into the placeholder as it arrives -
// the (potentially multi-GB) source is never written to the SD card as a
// single file. Used by install_nsp_native.c and install_xci_native.c. This
// is what lets a >4GB NSP/XCI install on a FAT32-formatted SD card (which
// caps any *single* file at 4GB): NCM already splits install content into
// separate per-NCA placeholder files, each ordinarily well under 4GB even
// for huge games - skipping the old "download the whole NSP/XCI to one file
// first" step means that per-NCA split is the only file size that matters
// anymore. A single NCA bigger than 4GB (rare) would still fail on FAT32;
// there's no way around that short of reformatting.
//
// Takes a ResolvedUrl* (not a raw URL string) so a whole install - one
// request per NCA - only pays a self-resolving proxy's resolve cost once
// instead of on every single content piece; see ResolvedUrl's doc comment
// in install_common.h.
//
// `out_registered` (may be NULL) reports whether this call actually
// registered new content (true) versus content_id was already present and
// nothing was written (false) - callers rolling back a failed/canceled
// install need this to know which content_ids are theirs to delete: rolling
// back a shared NCA that already belonged to some other, already-installed
// title would break that title too.
bool ncm_install_content_from_url(NcmContentStorage *cs, const NcmContentId *content_id,
                                   ResolvedUrl *ru, uint64_t file_offset, uint64_t size,
                                   InstallProgressCallback cb, void *userdata,
                                   bool *out_registered,
                                   char *err_buf, size_t err_buf_size);

// Reads the CNMT out of the just-installed `cnmt_content_id` by mounting it
// through fsOpenFileSystemWithId(..., FsFileSystemType_ContentMeta, ...) -
// the sanctioned way to read a Meta-type NCA's contents; the OS decrypts it
// transparently using hardware-sealed keys, so no key material is needed
// here. Fills `out`. Returns false on any I/O/parse error.
bool ncm_read_content_meta(NcmContentStorage *cs, const NcmContentId *cnmt_content_id, ContentMetaInfo *out,
                            char *err_buf, size_t err_buf_size);

// Assembles and commits the install content-meta record (header + extended
// header + the cnmt's own content record + `meta`'s content records) into
// `db`. Known limitation: content_meta_count > 0 (a meta referencing other
// metas) isn't handled - no catalog NSP encountered so far uses it.
bool ncm_commit_content_meta(NcmContentMetaDatabase *db, const ContentMetaInfo *meta,
                              const NcmContentInfo *cnmt_content_info,
                              char *err_buf, size_t err_buf_size);
