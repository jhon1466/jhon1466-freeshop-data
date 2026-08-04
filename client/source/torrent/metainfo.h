#pragma once
// Adapted from pipensx (https://github.com/i3sey/pipensx,
// src/core/metainfo.c/.h, GPL-3.0) - see ../../THIRD_PARTY_NOTICES.md.
#include <stddef.h>
#include <stdint.h>

#define MAX_TRACKERS 32
#define MAX_FILES 512
#define MAX_NAME_LEN 256
#define MAX_WEB_SEEDS 8

// Sanity ceilings for untrusted metadata. Real torrents top out around
// 16 MiB pieces; the cap only has to keep piece.c's per-piece allocation
// and the total-length arithmetic in a range that cannot overflow.
#define MAX_PIECE_LENGTH (256LL * 1024 * 1024)
#define MAX_TOTAL_LENGTH (1LL << 50) // 1 PiB

typedef struct {
    char path[MAX_NAME_LEN]; // relative path, '/'-separated
    int64_t length;
    int64_t offset; // byte offset in the torrent's flat data space
} mi_file_t;

typedef struct {
    // Identity
    uint8_t info_hash[20];
    char name[MAX_NAME_LEN];

    // Piece info - piece_hashes/num_pieces stay 0/NULL for a magnet whose
    // metadata hasn't been fetched from peers yet (see torrent.c's
    // ut_metadata handling); everything else here is populated once it has.
    int64_t piece_length;
    uint32_t num_pieces;
    uint8_t *piece_hashes; // num_pieces * 20 bytes, heap-allocated

    // Files
    int is_multi;
    int64_t total_length;
    uint32_t num_files;
    mi_file_t *files; // heap-allocated; for a single-file torrent, [0] is the file

    // Trackers (announce-list flattened)
    uint32_t num_trackers;
    char trackers[MAX_TRACKERS][512];

    // Web seeds (BEP-19 "url-list": GetRight-style HTTP/FTP payload mirrors)
    uint32_t num_web_seeds;
    char web_seeds[MAX_WEB_SEEDS][512];
} metainfo_t;

// Parses a .torrent file from memory. Returns 1 on success, 0 on error.
// Caller must call metainfo_free() when done (safe to call on a
// zero-initialized or already-freed metainfo_t).
int metainfo_parse(const uint8_t *data, size_t len, metainfo_t *mi);
void metainfo_free(metainfo_t *mi);

// Loads a .torrent file from an SD card path.
int metainfo_load(const char *path, metainfo_t *mi);

// Non-zero when a torrent-supplied path is safe to append to an output
// directory (no absolute path, no ".."/"." components, no backslashes).
int metainfo_path_is_safe(const char *path);
