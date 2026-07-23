#pragma once
#include <switch.h>
#include <stdio.h>
#include <stdbool.h>
#include "install.h"

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
bool ncm_install_content(NcmContentStorage *cs, const NcmContentId *content_id,
                          FILE *src, uint64_t file_offset, uint64_t size,
                          InstallProgressCallback cb, void *userdata,
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
