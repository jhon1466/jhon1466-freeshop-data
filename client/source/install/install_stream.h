#pragma once
#include "install_local.h" // InstallLocalResult - same outcomes, same meanings

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Installs an NSP or XCI straight from a byte stream, without ever staging
// the whole container on the SD card first - the counterpart to
// install_local.h's install_nsp_from_local_file/install_xci_from_local_file
// for sources that arrive push-only and can't be seeked, i.e. a host
// pushing a file over MTP (mtp_ptp.c). Two things make this possible even
// though the staged installer looks like it needs random access:
//
//   - Every entry's directory (offset/size/name) sits before its data: a
//     PFS0's (NSP) at the very start of the file; an XCI's twice over - the
//     root HFS0 locates the "secure" partition, whose own HFS0 then
//     directories its NCAs/cnmt/tik/cert - but always ahead of the bytes it
//     describes. By the time any content's data arrives, this already knows
//     where it goes.
//   - The CNMT is only needed to *commit* the meta record at the end, and
//     ncm_read_content_meta reads it back out of NCM storage after it's
//     installed - not out of the source container. So content can be
//     written in whatever order it physically appears.
//
// Everything else follows the staged installer exactly: same NCM
// placeholder writes, same ticket import, same application record push,
// same rollback-on-failure.
//
// XCI's "secure" partition (the one actually installed) is ordinarily the
// last and by far the largest thing in the file, so unlike an NSP, most of
// an XCI's bytes arrive before this can do anything with them - it's still
// worth using (no staging copy, no second install pass afterward), just
// don't expect the install-while-copying overlap NSP gets.
typedef struct InstallStream InstallStream;

// Opens the ncm/es/ns services and prepares to receive `total_size` bytes
// of an NSP. Returns NULL with a reason in err_buf on failure.
InstallStream *install_stream_begin(uint64_t total_size, char *err_buf, size_t err_buf_size);

// Same as install_stream_begin, but for an XCI.
InstallStream *install_stream_begin_xci(uint64_t total_size, char *err_buf, size_t err_buf_size);

// Feeds the next consecutive chunk of the container. Bytes are routed to
// the right NCM placeholder (or held aside, for the small .tik/.cert
// entries, or discarded, for XCI bytes outside the "secure" partition) as
// they arrive. Returns false with a reason in err_buf on the first error;
// the stream must then be abandoned with install_stream_abort.
bool install_stream_feed(InstallStream *s, const void *data, size_t len, char *err_buf, size_t err_buf_size);

// Completes the install: reads each CNMT back out of NCM, commits its meta
// record, pushes the application record, and imports tickets. Frees `s`
// either way - rolling back anything it registered if the result isn't
// INSTALL_LOCAL_OK.
InstallLocalResult install_stream_finish(InstallStream *s, char *err_buf, size_t err_buf_size);

// Abandons a stream (transfer canceled or failed), rolling back whatever
// content was already registered. Frees `s`. NULL-safe.
void install_stream_abort(InstallStream *s);
