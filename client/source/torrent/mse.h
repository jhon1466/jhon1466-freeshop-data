#pragma once
// Adapted from pipensx (https://github.com/i3sey/pipensx,
// src/core/mse.c/.h, GPL-3.0) - see ../../THIRD_PARTY_NOTICES.md.
//
// Message Stream Encryption / Protocol Encryption (MSE/PE) primitives -
// what lets this connect to peers/trackers that require or prefer
// obfuscated traffic (common on public trackers specifically to dodge ISP
// torrent throttling). Self-contained crypto for the outgoing-connection
// (initiator) side of the handshake: RC4 keystream, the fixed 768-bit
// Diffie-Hellman group, and the SHA-1 key derivation. The stateful
// handshake framing that uses these lives in peer.c; this file is pure and
// side-effect-free - no sockets - so it runs identically whether fed from
// the real non-blocking event loop or a test harness.
//
// Modular exponentiation goes through mbedtls (already linked into this
// client - see install/install_common.c's own SHA-256 use) rather than
// pipensx's own OpenSSL fallback, which this Switch-only client never needs.
#include <stddef.h>
#include <stdint.h>

#define MSE_DH_LEN 96 // 768-bit DH public key / shared secret, big-endian
#define MSE_VC_LEN 8  // verification constant: 8 zero bytes

// crypto_provide / crypto_select bitfield (the unofficial-but-universal MSE spec).
#define MSE_CRYPTO_PLAINTEXT 0x01u
#define MSE_CRYPTO_RC4 0x02u

// ---- RC4 ----
typedef struct {
    uint8_t s[256];
    uint8_t i, j;
} rc4_t;

void rc4_init(rc4_t *rc4, const uint8_t *key, size_t key_len);
// XORs `len` keystream bytes over in->out (in == out is allowed).
void rc4_crypt(rc4_t *rc4, const uint8_t *in, uint8_t *out, size_t len);
// Discards `n` keystream bytes (MSE requires discarding the first 1024).
void rc4_discard(rc4_t *rc4, size_t n);

// ---- Diffie-Hellman (g = 2, P = the MSE 768-bit prime) ----

// The MSE prime, big-endian, 96 bytes.
extern const uint8_t MSE_PRIME[MSE_DH_LEN];

// Generates a fresh private exponent. MSE specifies Xa/Xb as a 160-bit
// random integer, so `priv` is zero-padded to MSE_DH_LEN with 20 random
// low bytes - a wider exponent would still be valid DH, but costs ~5x the
// modexp, and both modexps of a connect run inline on torrent_step().
void mse_dh_private(uint8_t priv[MSE_DH_LEN]);

// pub = 2^priv mod P. Both are MSE_DH_LEN big-endian.
void mse_dh_public(const uint8_t priv[MSE_DH_LEN], uint8_t pub[MSE_DH_LEN]);

// secret = peer_pub^priv mod P, MSE_DH_LEN big-endian.
void mse_dh_secret(const uint8_t priv[MSE_DH_LEN], const uint8_t peer_pub[MSE_DH_LEN],
                    uint8_t secret[MSE_DH_LEN]);

// Derives an RC4 stream keyed by HASH(label | S | SKEY), where label is
// "keyA" (initiator's send stream) or "keyB" (initiator's receive stream),
// S is the DH shared secret, and SKEY is the 20-byte info hash. The
// returned stream has already discarded its first 1024 keystream bytes,
// per MSE.
void mse_stream_key(const char label[4], const uint8_t secret[MSE_DH_LEN], const uint8_t info_hash[20],
                     rc4_t *out);

// ---- Initiator handshake state machine ----
//
// Pure, buffer-driven: no sockets. The caller (peer.c) feeds bytes received
// from the socket and drains bytes to send, so the same code runs in the
// non-blocking event loop and in a loopback unit test. Only outgoing
// connections are supported (this responder only ever dials peers, never
// accepts inbound connections requiring MSE), so only the initiator side
// exists.
//
// Flow: start() emits pubA+padA; feed() consumes pubB, emits the encrypted
// request (with the BT handshake piggybacked as IA), resynchronises on the
// peer's encrypted verification constant, reads crypto_select, and reports
// MSE_DONE. After that, send_rc4/recv_rc4 encrypt/decrypt all further traffic.

#define MSE_MAX_PAD 512 // per spec: PadA..PadD are each 0..512 bytes
#define MSE_MAX_IA 68   // exactly the 68-byte BT handshake is piggybacked

typedef enum {
    MSE_CONTINUE = 0, // progress made / need more input - call feed() again
    MSE_DONE = 1,     // handshake complete; send_rc4/recv_rc4 are live
    MSE_FAIL = -1,    // peer isn't MSE/RC4 capable, or framing was invalid
} mse_status_t;

typedef struct {
    int state;
    uint8_t priv[MSE_DH_LEN];
    uint8_t info_hash[20];
    uint8_t ia[MSE_MAX_IA];
    size_t ia_len;

    rc4_t send_rc4; // keyA - our send stream (live once keys are derived)
    rc4_t recv_rc4; // keyB - our receive stream
    uint8_t vc_expect[MSE_VC_LEN]; // keyB keystream[0..7] - the encrypted VC

    uint32_t skipped; // bytes skipped during VC resync
    uint8_t select_buf[6]; // crypto_select(4) + len(padD)(2)
    uint32_t select_have;
    uint32_t padd_remaining;
} mse_client_t;

// Begins the handshake. `priv` is MSE_DH_LEN random bytes, `pad` is 0..512
// random bytes (padA), `ia` is the initial payload to piggyback (the BT
// handshake). Writes pubA+padA to `out`. Returns MSE_CONTINUE or MSE_FAIL.
mse_status_t mse_client_start(mse_client_t *c, const uint8_t info_hash[20], const uint8_t priv[MSE_DH_LEN],
                               const uint8_t *pad, size_t pad_len, const uint8_t *ia, size_t ia_len, uint8_t *out,
                               size_t out_cap, size_t *produced);

// Feeds received bytes. Sets *consumed to how many input bytes were used
// and writes any bytes to send into `out`/*produced. Returns MSE_CONTINUE
// until the handshake finishes (MSE_DONE) or fails (MSE_FAIL). Bytes left
// unconsumed must be presented again on the next call.
mse_status_t mse_client_feed(mse_client_t *c, const uint8_t *in, size_t in_len, size_t *consumed, uint8_t *out,
                              size_t out_cap, size_t *produced);
