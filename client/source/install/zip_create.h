#pragma once
#include "install.h"
#include <stdbool.h>
#include <stddef.h>

// Recursively zips every file under `src_dir` into a new archive at
// `zip_path` (created/overwritten) using Deflate compression - the write
// counterpart to zip_extract_to_dir.c's reader. No explicit directory
// entries are written (matching the reader's own comment: it already
// creates a file's parent directories from its path when the archive
// doesn't have one), and up to ZIP_CREATE_ENTRIES_MAX files are supported -
// past that the archive is aborted rather than silently left incomplete.
//
// `cb`/`userdata` report progress against the sum of every file's
// uncompressed size (a stat-only pre-pass, same shape as every other
// installer/extractor in this project) - returning false cancels, and the
// partially-written zip_path is deleted rather than left behind corrupt.
bool zip_create_from_dir(const char *src_dir, const char *zip_path,
                          InstallProgressCallback cb, void *userdata,
                          char *err_buf, size_t err_buf_size);
