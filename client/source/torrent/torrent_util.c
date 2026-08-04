// Adapted from pipensx (https://github.com/i3sey/pipensx,
// src/core/util.c, GPL-3.0) - see ../../THIRD_PARTY_NOTICES.md. Timing
// switched from POSIX clock_gettime to armGetSystemTick/armTicksToNs to
// match how the rest of this project already measures elapsed time
// (mtp_ptp.c, ftp_server.c) - same monotonic-since-boot semantics, just
// this codebase's usual primitive for it instead of a second one.
#include "torrent_util.h"
#include <switch.h>

uint64_t now_ms(void) {
    return armTicksToNs(armGetSystemTick()) / 1000000ULL;
}

uint64_t now_us(void) {
    return armTicksToNs(armGetSystemTick()) / 1000ULL;
}

time_t now_sec(void) {
    return (time_t)(now_ms() / 1000ULL);
}

void hex20(char buf[41], const uint8_t hash[20]) {
    static const char h[] = "0123456789abcdef";
    for (int i = 0; i < 20; i++) {
        buf[i * 2] = h[hash[i] >> 4];
        buf[i * 2 + 1] = h[hash[i] & 15];
    }
    buf[40] = '\0';
}

void rand_bytes(uint8_t *buf, size_t n) {
    randomGet(buf, n);
}
