#include "torrent_log.h"
#include <pthread.h>
#include <stdio.h>
#include <stdarg.h>

// The log file is opened once and kept open, rather than reopened per line.
//
// Every fopen() on Switch is a full filesystem round trip (fsdev_fstat ->
// svcSendSyncRequest), and this is called from the torrent engine's peer
// and DHT threads often enough that a busy transfer logs hundreds of lines.
// A crash report caught the install thread sitting inside exactly that
// fopen. Keeping the handle turns each line into a buffered write.
//
// The mutex is what makes that safe: the engine logs from several threads,
// and while newlib locks an individual FILE*, the open/close churn it
// replaces was serialising on the filesystem instead. Still flushed per
// line, so a hard crash doesn't lose the tail - which is the whole point of
// having this log.
static pthread_mutex_t s_mutex = PTHREAD_MUTEX_INITIALIZER;
static FILE *s_fp = NULL;
static int s_open_failed = 0;

void torrent_debug_log(const char *fmt, ...) {
    pthread_mutex_lock(&s_mutex);

    if (!s_fp && !s_open_failed) {
        s_fp = fopen("sdmc:/switch/freeshop/torrent_debug.log", "a");
        // Only retried on the next app launch: if the SD is read-only or
        // the path is missing, retrying per line would reintroduce exactly
        // the fopen storm this exists to avoid.
        if (!s_fp) s_open_failed = 1;
    }

    if (s_fp) {
        va_list ap;
        va_start(ap, fmt);
        vfprintf(s_fp, fmt, ap);
        va_end(ap);
        fputc('\n', s_fp);
        fflush(s_fp);
    }

    pthread_mutex_unlock(&s_mutex);
}
