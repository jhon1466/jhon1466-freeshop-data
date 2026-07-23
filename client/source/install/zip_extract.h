#pragma once
#include "install.h"
#include <stdbool.h>
#include <stddef.h>

// Extracts every entry in the ZIP at `zip_path` into `dest_dir` (created if
// missing), recreating whatever subdirectory structure the archive has -
// this is how a "port" (an .nro plus its data files/subfolders, e.g. a game
// WAD or asset directory) gets laid out under sdmc:/switch/<id>/. Supports
// the two compression methods every mainstream zip tool produces - Stored
// (0) and Deflate (8), decompressed via zlib's raw inflate (already linked
// for curl's TLS backend) - any other method is treated as unsupported and
// fails cleanly rather than silently skipping the entry.
//
// `cb`/`userdata` report progress against the sum of every entry's
// uncompressed size, same shape as every other installer in this project -
// returning false cancels (partial extraction is left in place, matching
// how a canceled download leaves a partial file rather than cleaning up,
// since the caller owns dest_dir's lifecycle either way).
bool zip_extract_to_dir(const char *zip_path, const char *dest_dir,
                         InstallProgressCallback cb, void *userdata,
                         char *err_buf, size_t err_buf_size);
