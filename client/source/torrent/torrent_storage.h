#pragma once
// Adapted from pipensx (https://github.com/i3sey/pipensx,
// src/platform/storage.c/.h, GPL-3.0) - see ../../THIRD_PARTY_NOTICES.md.
//
// Maps a torrent's flat piece-offset space onto its output files. Each file
// gets one of three modes:
//   - STORAGE_FILE_DISK: written to a real file on sdmc: (the default -
//     needed for multi-file torrents, and for anything kept around to seed).
//   - STORAGE_FILE_SINK: handed byte-by-byte to a caller-supplied callback
//     instead of touching disk - this is what lets a single-file NSP/XCI
//     torrent install straight from the piece stream via install_stream.c,
//     the same "install while copying" behavior MTP/FTP already have,
//     instead of staging a full copy to SD first.
//   - STORAGE_FILE_SKIP: dropped entirely (files the user didn't select to
//     download out of a multi-file torrent).
#include <stddef.h>
#include <stdint.h>
#include "metainfo.h"

typedef struct storage storage_t;

typedef enum {
    STORAGE_FILE_DISK = 0,
    STORAGE_FILE_SINK = 1,
    STORAGE_FILE_SKIP = 2,
} storage_file_mode_t;

typedef int (*storage_sink_fn)(void *user, uint32_t file_index,
                               int64_t file_offset,
                               const uint8_t *data, size_t len);

typedef struct {
    storage_file_mode_t mode;
    storage_sink_fn sink;
    void *user;
    // Prefix of a SINK file already consumed by a previous session's
    // installer (from an install-resume journal). Pieces that lie entirely
    // below this mark are reported as skipped (no re-download); a piece
    // straddling the mark is re-downloaded and only its tail at/after the
    // mark is delivered to the sink. 0 = plain sink behaviour.
    uint64_t ready_bytes;
} storage_file_config_t;

// Opens (creates) output files for a torrent. outdir is created if absent.
// Returns a handle or NULL on error.
storage_t *storage_open(const metainfo_t *mi, const char *outdir);
storage_t *storage_open_ex(const metainfo_t *mi, const char *outdir,
                           const storage_file_config_t *configs);

// Writes data at the absolute torrent byte offset. Returns 1 on success.
int storage_write(storage_t *s, int64_t offset, const uint8_t *data, size_t len);

// Flushes all DISK output files. Returns 1 on success.
int storage_flush(storage_t *s);

// Stronger than storage_flush: closes and reopens every DISK file so the
// bytes written so far are unambiguously committed and visible to a
// *separate* handle on the same file. fflush only empties stdio's own
// buffer, and fsync is best-effort (a devoptab that does not implement it
// silently does nothing), so neither is enough on its own for the one
// caller that needs this guarantee - a torrent install reading the
// container through its own handle while this one keeps writing (see
// install_torrent.c). Reopens with "r+b", so nothing is truncated.
// Returns 1 on success; on a failed reopen the file handle is left NULL
// and further writes to it fail rather than silently going nowhere.
int storage_commit(storage_t *s);

// Reads data at the absolute torrent byte offset (for seeding / verify).
// Returns bytes actually read, or -1 on error.
int storage_read(storage_t *s, int64_t offset, uint8_t *data, size_t len);

// True when the complete range is backed by ordinary DISK files.
int storage_range_readable(storage_t *s, int64_t offset, size_t len);

// True when the complete range is already processed: SKIP files plus the
// consumed (ready_bytes) prefix of SINK files.
int storage_range_skipped(storage_t *s, int64_t offset, size_t len);

const char *storage_error(storage_t *s);

// Returns the real on-disk path storage_open_ex resolved for file `index`
// (whichever of the "natural" mi->name/mf->path layout or a sanitized
// _files/NNNNNN_name fallback was actually used - RuTracker release names
// routinely blow past FAT path-length limits, so callers that need the
// downloaded file afterward - see install_torrent.c - MUST go through this
// rather than reconstructing the "natural" path themselves, which silently
// opens the wrong (nonexistent) file whenever the fallback triggered).
// Returns 1 on success, 0 if index is out of range or that file isn't
// DISK-backed (SINK/SKIP files were never given a path).
int storage_file_path(storage_t *s, uint32_t index, char *out, size_t out_size);

void storage_close(storage_t *s);
