#pragma once
// Diagnostic-only debug log for the torrent engine - same append-only,
// always-on pattern as net/http.h's download_debug_log (see that file's
// own doc comment for why: a real bug report that pointed at needing
// concrete on-disk evidence instead of guessing blind). `fmt` should not
// include a trailing newline - this adds one.
void torrent_debug_log(const char *fmt, ...);
