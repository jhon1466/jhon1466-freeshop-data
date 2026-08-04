#include "torrent_log.h"
#include <stdio.h>
#include <stdarg.h>

void torrent_debug_log(const char *fmt, ...) {
    FILE *fp = fopen("sdmc:/switch/freeshop/torrent_debug.log", "a");
    if (!fp) return;
    va_list ap;
    va_start(ap, fmt);
    vfprintf(fp, fmt, ap);
    va_end(ap);
    fputc('\n', fp);
    fclose(fp);
}
