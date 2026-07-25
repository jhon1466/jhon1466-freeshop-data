#pragma once
#include "install.h"
#include "install_common.h"

#include <switch.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Installs one NSZ-compressed content piece (a ".ncz" PFS0 entry) straight
// from the network into NCM content storage - the compressed/decompress
// counterpart to ncm_install_content_from_url() in ncm_install.h, which
// handles a plain ".nca" entry the same way. See ncz.c for the full NCZ
// format writeup (checked against nsz's own reference decompressor).
//
// `compressed_size` is the .ncz entry's own size as recorded in the PFS0
// (how many bytes to fetch over the network). `final_size` is the real,
// decompressed NCA size this content piece must end up as, from the CNMT's
// content_infos[i].size (see ncm_read_content_meta) - NOT the PFS0 size,
// which only knows the compressed size.
bool ncm_install_ncz_content_from_url(NcmContentStorage *cs, const NcmContentId *content_id,
                                       ResolvedUrl *ru, uint64_t entry_offset,
                                       uint64_t compressed_size, uint64_t final_size,
                                       InstallProgressCallback cb, void *userdata,
                                       bool *out_registered,
                                       char *err_buf, size_t err_buf_size);
