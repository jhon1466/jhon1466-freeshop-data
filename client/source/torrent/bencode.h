#pragma once
// Adapted from pipensx (https://github.com/i3sey/pipensx,
// src/core/bencode.c/.h, GPL-3.0) - see ../../THIRD_PARTY_NOTICES.md.
//
// Lightweight read-only bencode parser (the serialization format .torrent
// files, tracker responses, DHT messages, and the peer wire extension
// protocol all use). Works on a flat buffer with zero allocations - every
// be_node_t just points back into whatever buffer was parsed, so the
// caller owns the buffer's lifetime for as long as it keeps using the
// parsed nodes.
#include <stddef.h>
#include <stdint.h>

typedef enum {
    BE_INT = 'i',
    BE_STR = 's',
    BE_LIST = 'l',
    BE_DICT = 'd',
    BE_ERR = 0,
} be_type_t;

typedef struct {
    be_type_t type;
    const char *buf;    // start of this value in the source buffer
    size_t raw_len;      // byte length of the bencoded value (e.g. for hashing an info dict)

    int64_t ival;        // for BE_INT

    const char *sval;    // for BE_STR
    size_t slen;
} be_node_t;

// Parses one bencode value starting at *p. On success returns 1 and
// advances *p past the value; on error returns 0 (and *p is left in an
// unspecified position - callers should abandon the whole parse, not try
// to resume from wherever it stopped).
int be_decode(const char **p, const char *end, be_node_t *out);

// Looks up `key` in a bencoded dictionary. `dict_start`/`dict_end` delimit
// the raw bencoded dict, including its leading 'd' and trailing 'e'.
// Returns 1 and fills *val if found, 0 otherwise.
int be_dict_get(const char *dict_start, const char *dict_end,
                 const char *key, size_t klen, be_node_t *val);

// Dict iterator: call with *p pointing just after the dict's 'd'; fills
// *key/*klen/*val on each call and advances *p. Returns 1 while items
// remain, 0 at the closing 'e' or on error.
int be_dict_next(const char **p, const char *end,
                  const char **key, size_t *klen, be_node_t *val);

// List iterator: call with *p pointing just after the list's 'l'; fills
// *item and advances *p. Returns 1 while items remain, 0 at the closing
// 'e' or on error.
int be_list_next(const char **p, const char *end, be_node_t *item);
