#include "ftp_server.h"
#include "../install/install_local.h"
#include "../install/install_stream.h"

#include <switch.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/stat.h>
#include <dirent.h>
#include <fcntl.h>
#include <errno.h>
#include <unistd.h>
#include <time.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

// Generous enough for any real directory depth this console's SD ever has -
// matches ui_explorer.c's own EXPLORER_PATH_MAX convention.
#define FTP_PATH_MAX 512
#define FTP_REAL_PATH_MAX (FTP_PATH_MAX + 16) // + "sdmc:" and slack
#define FTP_LINE_MAX 600
#define FTP_TRANSFER_CHUNK (128 * 1024)
#define FTP_DATA_TIMEOUT_MS 10000

// ---- Session state - one client at a time, same design as mtp_ptp.c's
// single-active-transfer s_recv. ----

static bool s_running = false;
static int s_listen_fd = -1;

static bool s_connected = false;
static int s_ctrl_fd = -1;
static char s_ctrl_buf[FTP_LINE_MAX];
static size_t s_ctrl_buf_len = 0;
static bool s_close_requested = false; // QUIT was answered - ftp_step tears the connection down after

static char s_cwd[FTP_PATH_MAX] = "/";
static char s_rename_from[FTP_PATH_MAX] = "";
static bool s_have_rename_from = false;

static int s_pasv_listen_fd = -1;
static u16 s_pasv_port = 0;

static u32 s_local_ip = 0;       // raw, same byte-order convention nifm hands back
static char s_local_ip_str[24] = "";

// ---- Low-level socket helpers - every socket in this module is
// non-blocking; "waiting" is always a bounded poll-with-sleep loop, so a
// slow/stalled peer can never hang the caller past its own timeout. ----

static void set_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags >= 0) fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

static bool would_block(void) {
    return errno == EAGAIN || errno == EWOULDBLOCK;
}

static bool send_all(int fd, const void *data, size_t len, u64 timeout_ms) {
    const uint8_t *p = (const uint8_t *)data;
    size_t sent = 0;
    u64 start = armGetSystemTick();
    while (sent < len) {
        ssize_t n = send(fd, p + sent, len - sent, 0);
        if (n > 0) { sent += (size_t)n; continue; }
        if (n < 0 && would_block()) {
            if (armTicksToNs(armGetSystemTick() - start) > timeout_ms * 1000000ULL) return false;
            svcSleepThread(1000000ULL); // 1ms
            continue;
        }
        return false;
    }
    return true;
}

// Returns bytes received (>0), 0 on an orderly close, or -1 on a real error/
// timeout - callers never need to distinguish "nothing yet" from "closed"
// themselves, unlike a raw non-blocking recv().
static int recv_chunk(int fd, void *buf, size_t len, u64 timeout_ms) {
    u64 start = armGetSystemTick();
    while (true) {
        ssize_t n = recv(fd, buf, len, 0);
        if (n >= 0) return (int)n;
        if (would_block()) {
            if (armTicksToNs(armGetSystemTick() - start) > timeout_ms * 1000000ULL) return -1;
            svcSleepThread(1000000ULL);
            continue;
        }
        return -1;
    }
}

// ---- Control connection line buffering - FTP commands are CRLF-terminated
// ASCII lines; this accumulates whatever's arrived so far and hands back one
// complete line at a time, tolerating a bare LF (some clients/scripts send
// that instead of CRLF). ----

static bool ctrl_pop_line(char *out, size_t out_size) {
    for (size_t i = 0; i < s_ctrl_buf_len; i++) {
        if (s_ctrl_buf[i] != '\n') continue;
        size_t len = i;
        if (len > 0 && s_ctrl_buf[len - 1] == '\r') len--;
        if (len >= out_size) len = out_size - 1;
        memcpy(out, s_ctrl_buf, len);
        out[len] = '\0';
        size_t consumed = i + 1;
        memmove(s_ctrl_buf, s_ctrl_buf + consumed, s_ctrl_buf_len - consumed);
        s_ctrl_buf_len -= consumed;
        return true;
    }
    return false;
}

// Non-blocking - true and fills `out` if a complete command line is ready
// (from this call's read or one buffered earlier), false otherwise.
// `out_closed` is set if the connection closed or errored, in which case
// the caller should tear the session down.
static bool ctrl_poll_line(char *out, size_t out_size, bool *out_closed) {
    *out_closed = false;
    if (ctrl_pop_line(out, out_size)) return true;

    if (s_ctrl_buf_len >= sizeof(s_ctrl_buf) - 1) {
        *out_closed = true; // a "line" longer than any real FTP command - treat as fatal
        return false;
    }
    ssize_t n = recv(s_ctrl_fd, s_ctrl_buf + s_ctrl_buf_len, sizeof(s_ctrl_buf) - 1 - s_ctrl_buf_len, 0);
    if (n > 0) {
        s_ctrl_buf_len += (size_t)n;
        return ctrl_pop_line(out, out_size);
    }
    if (n == 0) { *out_closed = true; return false; }
    if (would_block()) return false;
    *out_closed = true;
    return false;
}

static bool ctrl_send_line(const char *text) {
    char buf[FTP_LINE_MAX + 20];
    int n = snprintf(buf, sizeof(buf), "%s\r\n", text);
    if (n < 0) return false;
    return send_all(s_ctrl_fd, buf, (size_t)n, 5000);
}

static bool ctrl_reply(int code, const char *msg) {
    char line[FTP_LINE_MAX];
    snprintf(line, sizeof(line), "%d %s", code, msg);
    return ctrl_send_line(line);
}

// ---- Path handling - the FTP root "/" maps to sdmc:/, giving full SD card
// access, same as this console's own SD file explorer. ----

// Resolves `arg` (absolute if it starts with '/', else relative to `cwd`)
// into a canonical absolute FTP path: always starts with '/', no "."/".."
// components, no trailing slash except the root itself.
static bool resolve_ftp_path(const char *cwd, const char *arg, char *out, size_t out_size) {
    char combined[FTP_PATH_MAX * 2];
    if (arg[0] == '/') snprintf(combined, sizeof(combined), "%s", arg);
    else snprintf(combined, sizeof(combined), "%s/%s", cwd, arg);

    char *stack[64];
    int depth = 0;
    char *save = NULL;
    char *tok = strtok_r(combined, "/", &save);
    while (tok) {
        if (strcmp(tok, ".") == 0) {
            // skip
        } else if (strcmp(tok, "..") == 0) {
            if (depth > 0) depth--;
        } else {
            if (depth >= 64) return false; // absurdly deep path
            stack[depth++] = tok;
        }
        tok = strtok_r(NULL, "/", &save);
    }

    char result[FTP_PATH_MAX] = "";
    size_t pos = 0;
    for (int i = 0; i < depth; i++) {
        int n = snprintf(result + pos, sizeof(result) - pos, "/%s", stack[i]);
        if (n < 0 || (size_t)n >= sizeof(result) - pos) return false; // too long
        pos += (size_t)n;
    }
    if (pos == 0) snprintf(result, sizeof(result), "/");

    snprintf(out, out_size, "%s", result);
    return true;
}

static void ftp_to_real_path(const char *ftp_path, char *out, size_t out_size) {
    snprintf(out, out_size, "sdmc:%s", ftp_path); // ftp_path always starts with '/', root included
}

static const char *ftp_basename(const char *ftp_path) {
    const char *slash = strrchr(ftp_path, '/');
    return slash ? slash + 1 : ftp_path;
}

// .nsp/.nsz install as a plain PFS0; .xci/.xcz through the nested-partition
// state machine - same four extensions and the same reasoning as
// mtp_ptp.c's own is_nsp/is_xci split, which install_stream.h's two entry
// points mirror exactly.
static bool is_installable(const char *filename, bool *out_is_xci) {
    const char *ext = strrchr(filename, '.');
    if (!ext) return false;
    if (strcasecmp(ext, ".nsp") == 0 || strcasecmp(ext, ".nsz") == 0) { *out_is_xci = false; return true; }
    if (strcasecmp(ext, ".xci") == 0 || strcasecmp(ext, ".xcz") == 0) { *out_is_xci = true; return true; }
    return false;
}

// ---- PASV/EPSV data connections - one at a time, opened by PASV/EPSV and
// consumed by the very next LIST/NLST/RETR/STOR. ----

static void pasv_close_listener(void) {
    if (s_pasv_listen_fd >= 0) { close(s_pasv_listen_fd); s_pasv_listen_fd = -1; }
}

// The address every listening socket in this module binds to - the
// console's own specific address (s_local_ip, from nifm), not the
// INADDR_ANY wildcard. mtheall/ftpd - a real, hardware-proven Switch FTP
// server - deliberately does the same (source/switch/platform.cpp's
// networkAddress() binds to gethostid(), which on this platform *is* the
// console's own address) rather than the wildcard every other platform it
// supports uses; the Switch's own network stack apparently doesn't route
// inbound connections to a wildcard-bound listening socket reliably, only
// to one bound to the specific interface address. Falls back to
// INADDR_ANY only if the address genuinely isn't known yet (shouldn't
// happen in practice - ftp_start doesn't get this far without one).
static u32 listen_bind_address(void) {
    return s_local_ip != 0 ? s_local_ip : (u32)INADDR_ANY;
}

static bool pasv_open(void) {
    pasv_close_listener(); // a PASV with no follow-up data command just leaks into the next one

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return false;
    int yes = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = listen_bind_address();
    addr.sin_port = 0; // ephemeral - let the OS pick
    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) { close(fd); return false; }
    if (listen(fd, 1) != 0) { close(fd); return false; }

    socklen_t alen = sizeof(addr);
    if (getsockname(fd, (struct sockaddr *)&addr, &alen) != 0) { close(fd); return false; }

    set_nonblocking(fd);
    s_pasv_listen_fd = fd;
    s_pasv_port = ntohs(addr.sin_port);
    return true;
}

// Waits (bounded) for the client to connect its data socket, and closes the
// listener either way - it's single-use regardless of outcome. -1 on
// timeout/error/no PASV having been issued.
static int pasv_accept(u64 timeout_ms) {
    if (s_pasv_listen_fd < 0) return -1;

    u64 start = armGetSystemTick();
    int fd = -1;
    while (true) {
        fd = accept(s_pasv_listen_fd, NULL, NULL);
        if (fd >= 0) break;
        if (would_block()) {
            if (armTicksToNs(armGetSystemTick() - start) > timeout_ms * 1000000ULL) break;
            svcSleepThread(2000000ULL); // 2ms
            continue;
        }
        break;
    }
    pasv_close_listener();
    if (fd >= 0) set_nonblocking(fd);
    return fd;
}

static void handle_pasv(bool extended) {
    if (!pasv_open()) { ctrl_reply(425, "Can't open passive connection."); return; }

    if (extended) {
        char msg[64];
        snprintf(msg, sizeof(msg), "Entering Extended Passive Mode (|||%u|)", s_pasv_port);
        ctrl_reply(229, msg);
    } else {
        char msg[64];
        snprintf(msg, sizeof(msg), "Entering Passive Mode (%u,%u,%u,%u,%u,%u)",
                 s_local_ip & 0xFF, (s_local_ip >> 8) & 0xFF, (s_local_ip >> 16) & 0xFF, (s_local_ip >> 24) & 0xFF,
                 (s_pasv_port >> 8) & 0xFF, s_pasv_port & 0xFF);
        ctrl_reply(227, msg);
    }
}

// ---- Directory listing (LIST) - classic `ls -l` format, the de facto
// standard every FTP client's parser is actually built around even though
// it was never part of the RFC. ----

static void format_ls_time(time_t mtime, char *out, size_t out_size) {
    static const char *months[] = { "Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                     "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };
    struct tm tm_mtime;
    gmtime_r(&mtime, &tm_mtime);
    double age_days = difftime(time(NULL), mtime) / 86400.0;
    int mon = tm_mtime.tm_mon % 12;
    if (mon < 0) mon = 0;
    // Recent files show a time, older ones a year - the same "is this file
    // less than ~6 months old" heuristic real `ls -l` uses, which is exactly
    // what lets a client's parser tell the two column formats apart.
    if (age_days >= 0 && age_days < 182) {
        snprintf(out, out_size, "%s %02d %02d:%02d", months[mon], tm_mtime.tm_mday, tm_mtime.tm_hour, tm_mtime.tm_min);
    } else {
        snprintf(out, out_size, "%s %02d %5d", months[mon], tm_mtime.tm_mday, tm_mtime.tm_year + 1900);
    }
}

static void format_list_line(const char *name, bool is_dir, long long size, time_t mtime,
                              char *out, size_t out_size) {
    char time_str[24];
    format_ls_time(mtime, time_str, sizeof(time_str));
    snprintf(out, out_size, "%crwxr-xr-x 1 switch switch %12lld %s %s",
             is_dir ? 'd' : '-', size, time_str, name);
}

static void handle_list_impl(const char *arg, bool names_only) {
    if (s_pasv_listen_fd < 0) { ctrl_reply(425, "Use PASV first."); return; }

    char target_ftp[FTP_PATH_MAX];
    if (arg[0] != '\0') {
        if (!resolve_ftp_path(s_cwd, arg, target_ftp, sizeof(target_ftp))) {
            ctrl_reply(501, "Invalid path.");
            pasv_close_listener();
            return;
        }
    } else {
        snprintf(target_ftp, sizeof(target_ftp), "%s", s_cwd);
    }
    char real_path[FTP_REAL_PATH_MAX];
    ftp_to_real_path(target_ftp, real_path, sizeof(real_path));

    DIR *dir = opendir(real_path);
    if (!dir) { ctrl_reply(550, "Failed to open directory."); pasv_close_listener(); return; }

    int data_fd = pasv_accept(FTP_DATA_TIMEOUT_MS);
    if (data_fd < 0) { closedir(dir); ctrl_reply(425, "Can't open data connection."); return; }

    ctrl_reply(150, "Here comes the directory listing.");

    struct dirent *ent;
    bool ok = true;
    while (ok && (ent = readdir(dir)) != NULL) {
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) continue;

        char rendered[FTP_LINE_MAX];
        if (names_only) {
            snprintf(rendered, sizeof(rendered), "%s", ent->d_name);
        } else {
            char child_real[FTP_REAL_PATH_MAX + 260];
            snprintf(child_real, sizeof(child_real), "%s/%s", real_path, ent->d_name);
            struct stat st;
            if (stat(child_real, &st) != 0) continue;
            format_list_line(ent->d_name, S_ISDIR(st.st_mode), (long long)st.st_size, st.st_mtime,
                              rendered, sizeof(rendered));
        }

        char with_crlf[FTP_LINE_MAX + 4];
        int n = snprintf(with_crlf, sizeof(with_crlf), "%s\r\n", rendered);
        if (n < 0 || !send_all(data_fd, with_crlf, (size_t)n, FTP_DATA_TIMEOUT_MS)) ok = false;
    }
    closedir(dir);
    close(data_fd);
    ctrl_reply(226, ok ? "Directory send OK." : "Transfer failed.");
}

// ---- Session history ----

static void push_ftp_history(FtpState *state, const char *filename, FtpHistoryStatus status, const char *error) {
    if (state->history_count == FTP_HISTORY_MAX) {
        memmove(&state->history[0], &state->history[1], sizeof(FtpHistoryItem) * (FTP_HISTORY_MAX - 1));
        state->history_count--;
    }
    FtpHistoryItem *item = &state->history[state->history_count++];
    snprintf(item->filename, sizeof(item->filename), "%s", filename);
    item->status = status;
    item->error[0] = '\0';
    if (error) snprintf(item->error, sizeof(item->error), "%s", error);
}

// ---- RETR (download) ----

static void handle_retr(const char *arg, FtpState *state, InstallProgressCallback progress_cb, void *userdata) {
    if (s_pasv_listen_fd < 0) { ctrl_reply(425, "Use PASV first."); return; }
    if (arg[0] == '\0') { ctrl_reply(501, "Missing filename."); pasv_close_listener(); return; }

    char target_ftp[FTP_PATH_MAX];
    if (!resolve_ftp_path(s_cwd, arg, target_ftp, sizeof(target_ftp))) {
        ctrl_reply(501, "Invalid path.");
        pasv_close_listener();
        return;
    }
    char real_path[FTP_REAL_PATH_MAX];
    ftp_to_real_path(target_ftp, real_path, sizeof(real_path));

    struct stat st;
    if (stat(real_path, &st) != 0 || !S_ISREG(st.st_mode)) {
        ctrl_reply(550, "File not found.");
        pasv_close_listener();
        return;
    }
    FILE *fp = fopen(real_path, "rb");
    if (!fp) { ctrl_reply(550, "Failed to open file."); pasv_close_listener(); return; }

    int data_fd = pasv_accept(FTP_DATA_TIMEOUT_MS);
    if (data_fd < 0) { fclose(fp); ctrl_reply(425, "Can't open data connection."); return; }

    ctrl_reply(150, "Opening data connection.");

    const char *filename = ftp_basename(target_ftp);
    snprintf(state->current_file, sizeof(state->current_file), "%s", filename);
    state->status = FTP_STATUS_TRANSFERRING;

    static uint8_t buf[FTP_TRANSFER_CHUNK];
    long total = (long)st.st_size;
    long done = 0;
    bool ok = true, canceled = false;

    while (true) {
        size_t n = fread(buf, 1, sizeof(buf), fp);
        if (n == 0) break;
        if (!send_all(data_fd, buf, n, FTP_DATA_TIMEOUT_MS)) { ok = false; break; }
        done += (long)n;
        if (progress_cb && !progress_cb(total, done, userdata)) { canceled = true; ok = false; break; }
        if (n < sizeof(buf)) break; // short read - end of file
    }
    fclose(fp);
    close(data_fd);

    if (ok) {
        push_ftp_history(state, filename, FTP_HISTORY_DOWNLOADED, NULL);
        ctrl_reply(226, "Transfer complete.");
    } else {
        push_ftp_history(state, filename, FTP_HISTORY_FAILED, canceled ? "cancelled" : "transfer interrupted");
        ctrl_reply(canceled ? 426 : 550, canceled ? "Transfer aborted." : "Transfer failed.");
    }
    state->current_file[0] = '\0';
}

// ---- STOR (upload) - installs straight from the transfer for
// .nsp/.nsz/.xci/.xcz (see ftp_server.h), writes a plain file otherwise. ----

typedef struct {
    InstallStream *stream; // direct-install sink; NULL when writing a plain file
    FILE *fp;
    char err[200];
} FtpSink;

static bool ftp_sink_write(FtpSink *sink, const uint8_t *data, size_t len) {
    if (len == 0) return true;
    if (sink->stream) return install_stream_feed(sink->stream, data, len, sink->err, sizeof(sink->err));
    return fwrite(data, 1, len, sink->fp) == len;
}

static void handle_stor(const char *arg, FtpState *state, InstallProgressCallback progress_cb, void *userdata) {
    if (s_pasv_listen_fd < 0) { ctrl_reply(425, "Use PASV first."); return; }
    if (arg[0] == '\0') { ctrl_reply(501, "Missing filename."); pasv_close_listener(); return; }

    char target_ftp[FTP_PATH_MAX];
    if (!resolve_ftp_path(s_cwd, arg, target_ftp, sizeof(target_ftp))) {
        ctrl_reply(501, "Invalid path.");
        pasv_close_listener();
        return;
    }
    const char *filename = ftp_basename(target_ftp);
    if (filename[0] == '\0') { ctrl_reply(501, "Invalid filename."); pasv_close_listener(); return; }

    bool is_xci = false;
    bool installable = is_installable(filename, &is_xci);

    FtpSink sink;
    memset(&sink, 0, sizeof(sink));
    if (installable) {
        // FTP has no equivalent of MTP's SendObjectInfo declaring a size
        // upfront - STOR just streams until the data connection closes -
        // so total_size is always unknown (0) here. install_stream.h
        // already treats that as "don't cap the XCI root-search window",
        // exactly like mtp_ptp.c's own >4GB path does.
        char err[200];
        sink.stream = is_xci ? install_stream_begin_xci(0, err, sizeof(err))
                              : install_stream_begin(0, err, sizeof(err));
        if (!sink.stream) { ctrl_reply(550, "Couldn't start install."); pasv_close_listener(); return; }
    } else {
        char real_path[FTP_REAL_PATH_MAX];
        ftp_to_real_path(target_ftp, real_path, sizeof(real_path));
        sink.fp = fopen(real_path, "wb");
        if (!sink.fp) { ctrl_reply(550, "Failed to open file."); pasv_close_listener(); return; }
    }

    int data_fd = pasv_accept(FTP_DATA_TIMEOUT_MS);
    if (data_fd < 0) {
        if (sink.stream) install_stream_abort(sink.stream);
        if (sink.fp) fclose(sink.fp);
        ctrl_reply(425, "Can't open data connection.");
        return;
    }

    ctrl_reply(150, "Ok to send data.");

    snprintf(state->current_file, sizeof(state->current_file), "%s", filename);
    state->status = FTP_STATUS_TRANSFERRING;

    static uint8_t buf[FTP_TRANSFER_CHUNK];
    long done = 0;
    bool ok = true, canceled = false;

    // The data connection closing is the transfer's own natural end - FTP
    // needs no declared size or sentinel the way MTP's SendObject does.
    while (true) {
        int n = recv_chunk(data_fd, buf, sizeof(buf), FTP_DATA_TIMEOUT_MS);
        if (n < 0) { ok = false; break; }
        if (n == 0) break;
        if (!ftp_sink_write(&sink, buf, (size_t)n)) { ok = false; break; }
        done += n;
        if (progress_cb && !progress_cb(0, done, userdata)) { canceled = true; ok = false; break; }
    }
    close(data_fd);

    if (!ok) {
        if (sink.stream) install_stream_abort(sink.stream);
        if (sink.fp) fclose(sink.fp);
        push_ftp_history(state, filename, FTP_HISTORY_FAILED,
                          canceled ? "cancelled" : (sink.err[0] != '\0' ? sink.err : "transfer interrupted"));
        state->current_file[0] = '\0';
        ctrl_reply(canceled ? 426 : 550, canceled ? "Transfer aborted." : "Transfer failed.");
        return;
    }

    if (sink.stream) {
        // Committing takes a few seconds with no control traffic serviced -
        // same UX treatment as mtp_ptp.c's finish_recv.
        state->status = FTP_STATUS_INSTALLING;
        if (progress_cb) progress_cb(0, 0, userdata);

        char err[200] = "";
        InstallLocalResult res = install_stream_finish(sink.stream, err, sizeof(err));
        if (res == INSTALL_LOCAL_OK) {
            push_ftp_history(state, filename, FTP_HISTORY_INSTALLED, NULL);
            ctrl_reply(226, "Transfer complete - installed.");
        } else if (res == INSTALL_LOCAL_ERR_CANCELED) {
            ctrl_reply(426, "Transfer aborted.");
        } else {
            push_ftp_history(state, filename, FTP_HISTORY_FAILED, err);
            ctrl_reply(550, "Install failed.");
        }
    } else {
        fclose(sink.fp);
        push_ftp_history(state, filename, FTP_HISTORY_UPLOADED, NULL);
        ctrl_reply(226, "Transfer complete.");
    }
    state->current_file[0] = '\0';
}

// ---- Remaining filesystem commands ----

static void handle_mkd(const char *arg) {
    if (arg[0] == '\0') { ctrl_reply(501, "Missing path."); return; }
    char target[FTP_PATH_MAX], real_path[FTP_REAL_PATH_MAX];
    if (!resolve_ftp_path(s_cwd, arg, target, sizeof(target))) { ctrl_reply(501, "Invalid path."); return; }
    ftp_to_real_path(target, real_path, sizeof(real_path));
    if (mkdir(real_path, 0777) != 0) { ctrl_reply(550, "Failed to create directory."); return; }
    char msg[FTP_PATH_MAX + 16];
    snprintf(msg, sizeof(msg), "\"%s\" created.", target);
    ctrl_reply(257, msg);
}

static void handle_rmd(const char *arg) {
    if (arg[0] == '\0') { ctrl_reply(501, "Missing path."); return; }
    char target[FTP_PATH_MAX], real_path[FTP_REAL_PATH_MAX];
    if (!resolve_ftp_path(s_cwd, arg, target, sizeof(target))) { ctrl_reply(501, "Invalid path."); return; }
    if (strcmp(target, "/") == 0) { ctrl_reply(550, "Cannot remove the root directory."); return; }
    ftp_to_real_path(target, real_path, sizeof(real_path));
    // Standard (non-recursive) RMD semantics - fails on a non-empty
    // directory rather than silently taking everything in it with it.
    if (rmdir(real_path) != 0) { ctrl_reply(550, "Failed to remove directory (not empty?)."); return; }
    ctrl_reply(250, "Directory removed.");
}

static void handle_dele(const char *arg) {
    if (arg[0] == '\0') { ctrl_reply(501, "Missing path."); return; }
    char target[FTP_PATH_MAX], real_path[FTP_REAL_PATH_MAX];
    if (!resolve_ftp_path(s_cwd, arg, target, sizeof(target))) { ctrl_reply(501, "Invalid path."); return; }
    ftp_to_real_path(target, real_path, sizeof(real_path));
    struct stat st;
    if (stat(real_path, &st) != 0 || !S_ISREG(st.st_mode)) { ctrl_reply(550, "File not found."); return; }
    if (remove(real_path) != 0) { ctrl_reply(550, "Failed to delete file."); return; }
    ctrl_reply(250, "File deleted.");
}

static void handle_rnfr(const char *arg) {
    if (arg[0] == '\0') { ctrl_reply(501, "Missing path."); return; }
    char target[FTP_PATH_MAX];
    if (!resolve_ftp_path(s_cwd, arg, target, sizeof(target))) { ctrl_reply(501, "Invalid path."); return; }
    char real_path[FTP_REAL_PATH_MAX];
    ftp_to_real_path(target, real_path, sizeof(real_path));
    struct stat st;
    if (stat(real_path, &st) != 0) { ctrl_reply(550, "File not found."); return; }
    snprintf(s_rename_from, sizeof(s_rename_from), "%s", target);
    s_have_rename_from = true;
    ctrl_reply(350, "Ready for RNTO.");
}

static void handle_rnto(const char *arg) {
    if (!s_have_rename_from) { ctrl_reply(503, "RNFR required first."); return; }
    s_have_rename_from = false;
    if (arg[0] == '\0') { ctrl_reply(501, "Missing path."); return; }
    char target[FTP_PATH_MAX];
    if (!resolve_ftp_path(s_cwd, arg, target, sizeof(target))) { ctrl_reply(501, "Invalid path."); return; }
    char real_from[FTP_REAL_PATH_MAX], real_to[FTP_REAL_PATH_MAX];
    ftp_to_real_path(s_rename_from, real_from, sizeof(real_from));
    ftp_to_real_path(target, real_to, sizeof(real_to));
    if (rename(real_from, real_to) != 0) { ctrl_reply(550, "Rename failed."); return; }
    ctrl_reply(250, "Rename successful.");
}

static void handle_size(const char *arg) {
    if (arg[0] == '\0') { ctrl_reply(501, "Missing path."); return; }
    char target[FTP_PATH_MAX], real_path[FTP_REAL_PATH_MAX];
    if (!resolve_ftp_path(s_cwd, arg, target, sizeof(target))) { ctrl_reply(501, "Invalid path."); return; }
    ftp_to_real_path(target, real_path, sizeof(real_path));
    struct stat st;
    if (stat(real_path, &st) != 0 || !S_ISREG(st.st_mode)) { ctrl_reply(550, "File not found."); return; }
    char msg[32];
    snprintf(msg, sizeof(msg), "%lld", (long long)st.st_size);
    ctrl_reply(213, msg);
}

static void handle_mdtm(const char *arg) {
    if (arg[0] == '\0') { ctrl_reply(501, "Missing path."); return; }
    char target[FTP_PATH_MAX], real_path[FTP_REAL_PATH_MAX];
    if (!resolve_ftp_path(s_cwd, arg, target, sizeof(target))) { ctrl_reply(501, "Invalid path."); return; }
    ftp_to_real_path(target, real_path, sizeof(real_path));
    struct stat st;
    if (stat(real_path, &st) != 0) { ctrl_reply(550, "File not found."); return; }
    struct tm tmv;
    gmtime_r(&st.st_mtime, &tmv);
    char msg[24];
    snprintf(msg, sizeof(msg), "%04d%02d%02d%02d%02d%02d",
             tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday, tmv.tm_hour, tmv.tm_min, tmv.tm_sec);
    ctrl_reply(213, msg);
}

// ---- Command dispatch ----

static void handle_line(const char *line, FtpState *state, InstallProgressCallback progress_cb, void *userdata) {
    char verb[16] = "";
    const char *arg = "";
    const char *sp = strchr(line, ' ');
    if (sp) {
        size_t vlen = (size_t)(sp - line);
        if (vlen >= sizeof(verb)) vlen = sizeof(verb) - 1;
        memcpy(verb, line, vlen);
        verb[vlen] = '\0';
        arg = sp + 1;
    } else {
        snprintf(verb, sizeof(verb), "%s", line);
    }

    if (strcasecmp(verb, "USER") == 0) {
        ctrl_reply(331, "User name okay, need password.");
    } else if (strcasecmp(verb, "PASS") == 0) {
        ctrl_reply(230, "Login successful.");
    } else if (strcasecmp(verb, "SYST") == 0) {
        ctrl_reply(215, "UNIX Type: L8");
    } else if (strcasecmp(verb, "FEAT") == 0) {
        ctrl_send_line("211-Features:");
        ctrl_send_line(" PASV");
        ctrl_send_line(" EPSV");
        ctrl_send_line(" SIZE");
        ctrl_send_line(" MDTM");
        ctrl_send_line(" UTF8");
        ctrl_reply(211, "End");
    } else if (strcasecmp(verb, "OPTS") == 0) {
        ctrl_reply(200, "OK.");
    } else if (strcasecmp(verb, "TYPE") == 0) {
        ctrl_reply(200, "Type set to I."); // everything here is already binary-safe regardless of A/I
    } else if (strcasecmp(verb, "PWD") == 0 || strcasecmp(verb, "XPWD") == 0) {
        char msg[FTP_PATH_MAX + 16];
        snprintf(msg, sizeof(msg), "\"%s\" is the current directory.", s_cwd);
        ctrl_reply(257, msg);
    } else if (strcasecmp(verb, "CWD") == 0 || strcasecmp(verb, "XCWD") == 0) {
        if (arg[0] == '\0') { ctrl_reply(501, "Missing path."); }
        else {
            char target[FTP_PATH_MAX], real_path[FTP_REAL_PATH_MAX];
            struct stat st;
            if (!resolve_ftp_path(s_cwd, arg, target, sizeof(target))) {
                ctrl_reply(501, "Invalid path.");
            } else {
                ftp_to_real_path(target, real_path, sizeof(real_path));
                if (stat(real_path, &st) == 0 && S_ISDIR(st.st_mode)) {
                    snprintf(s_cwd, sizeof(s_cwd), "%s", target);
                    ctrl_reply(250, "Directory successfully changed.");
                } else {
                    ctrl_reply(550, "Failed to change directory.");
                }
            }
        }
    } else if (strcasecmp(verb, "CDUP") == 0 || strcasecmp(verb, "XCUP") == 0) {
        char target[FTP_PATH_MAX];
        if (resolve_ftp_path(s_cwd, "..", target, sizeof(target))) snprintf(s_cwd, sizeof(s_cwd), "%s", target);
        ctrl_reply(250, "Directory successfully changed.");
    } else if (strcasecmp(verb, "PASV") == 0) {
        handle_pasv(false);
    } else if (strcasecmp(verb, "EPSV") == 0) {
        handle_pasv(true);
    } else if (strcasecmp(verb, "PORT") == 0 || strcasecmp(verb, "EPRT") == 0) {
        ctrl_reply(502, "Active mode not supported - use passive mode (PASV/EPSV).");
    } else if (strcasecmp(verb, "LIST") == 0) {
        handle_list_impl(arg, false);
    } else if (strcasecmp(verb, "NLST") == 0) {
        handle_list_impl(arg, true);
    } else if (strcasecmp(verb, "RETR") == 0) {
        handle_retr(arg, state, progress_cb, userdata);
    } else if (strcasecmp(verb, "STOR") == 0) {
        handle_stor(arg, state, progress_cb, userdata);
    } else if (strcasecmp(verb, "DELE") == 0) {
        handle_dele(arg);
    } else if (strcasecmp(verb, "RMD") == 0 || strcasecmp(verb, "XRMD") == 0) {
        handle_rmd(arg);
    } else if (strcasecmp(verb, "MKD") == 0 || strcasecmp(verb, "XMKD") == 0) {
        handle_mkd(arg);
    } else if (strcasecmp(verb, "RNFR") == 0) {
        handle_rnfr(arg);
    } else if (strcasecmp(verb, "RNTO") == 0) {
        handle_rnto(arg);
    } else if (strcasecmp(verb, "SIZE") == 0) {
        handle_size(arg);
    } else if (strcasecmp(verb, "MDTM") == 0) {
        handle_mdtm(arg);
    } else if (strcasecmp(verb, "NOOP") == 0) {
        ctrl_reply(200, "OK.");
    } else if (strcasecmp(verb, "ABOR") == 0) {
        pasv_close_listener();
        ctrl_reply(226, "ABOR command successful.");
    } else if (strcasecmp(verb, "QUIT") == 0) {
        ctrl_reply(221, "Goodbye.");
        s_close_requested = true;
    } else {
        ctrl_reply(502, "Command not implemented.");
    }
}

// ---- Connection lifecycle ----

static void close_client(void) {
    pasv_close_listener();
    if (s_ctrl_fd >= 0) { close(s_ctrl_fd); s_ctrl_fd = -1; }
    s_connected = false;
    s_ctrl_buf_len = 0;
    s_have_rename_from = false;
    s_close_requested = false;
    snprintf(s_cwd, sizeof(s_cwd), "/");
}

bool ftp_start(char *err_buf, size_t err_buf_size) {
    if (err_buf && err_buf_size > 0) err_buf[0] = '\0';

    Result rc = nifmInitialize(NifmServiceType_User);
    if (R_FAILED(rc)) {
        if (err_buf) snprintf(err_buf, err_buf_size, "no se pudo conectar con el servicio nifm (0x%x)", rc);
        return false;
    }

    s_local_ip = 0;
    s_local_ip_str[0] = '\0';
    u32 ip = 0;
    if (R_SUCCEEDED(nifmGetCurrentIpAddress(&ip)) && ip != 0) {
        s_local_ip = ip;
        snprintf(s_local_ip_str, sizeof(s_local_ip_str), "%u.%u.%u.%u",
                 ip & 0xFF, (ip >> 8) & 0xFF, (ip >> 16) & 0xFF, (ip >> 24) & 0xFF);
    }

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        if (err_buf) snprintf(err_buf, err_buf_size, "socket() falló");
        nifmExit();
        return false;
    }
    int yes = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = listen_bind_address();
    addr.sin_port = htons(FTP_SERVER_PORT);
    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        if (err_buf) snprintf(err_buf, err_buf_size, "no se pudo abrir el puerto %d (¿ya en uso?)", FTP_SERVER_PORT);
        close(fd);
        nifmExit();
        return false;
    }
    if (listen(fd, 1) != 0) {
        if (err_buf) snprintf(err_buf, err_buf_size, "listen() falló");
        close(fd);
        nifmExit();
        return false;
    }
    set_nonblocking(fd);

    s_listen_fd = fd;
    s_connected = false;
    s_running = true;
    snprintf(s_cwd, sizeof(s_cwd), "/");
    return true;
}

void ftp_stop(void) {
    close_client();
    if (s_listen_fd >= 0) { close(s_listen_fd); s_listen_fd = -1; }
    nifmExit();
    s_running = false;
}

void ftp_step(FtpState *out, InstallProgressCallback progress_cb, void *userdata) {
    if (!s_running) {
        out->status = FTP_STATUS_WAITING_FOR_NETWORK;
        return;
    }

    if (s_local_ip_str[0] == '\0') {
        // Wi-Fi/Ethernet may connect after this screen was already opened -
        // keep checking rather than only trying once at ftp_start.
        u32 ip = 0;
        if (R_SUCCEEDED(nifmGetCurrentIpAddress(&ip)) && ip != 0) {
            s_local_ip = ip;
            snprintf(s_local_ip_str, sizeof(s_local_ip_str), "%u.%u.%u.%u",
                     ip & 0xFF, (ip >> 8) & 0xFF, (ip >> 16) & 0xFF, (ip >> 24) & 0xFF);
        } else {
            out->status = FTP_STATUS_WAITING_FOR_NETWORK;
            out->local_ip[0] = '\0';
            return;
        }
    }
    snprintf(out->local_ip, sizeof(out->local_ip), "%s", s_local_ip_str);

    if (!s_connected) {
        int fd = accept(s_listen_fd, NULL, NULL);
        if (fd >= 0) {
            set_nonblocking(fd);
            s_ctrl_fd = fd;
            s_connected = true;
            s_ctrl_buf_len = 0;
            s_close_requested = false;
            s_have_rename_from = false;
            snprintf(s_cwd, sizeof(s_cwd), "/");

            struct sockaddr_in peer;
            socklen_t plen = sizeof(peer);
            out->client_ip[0] = '\0';
            if (getpeername(fd, (struct sockaddr *)&peer, &plen) == 0) {
                inet_ntop(AF_INET, &peer.sin_addr, out->client_ip, sizeof(out->client_ip));
            }
            ctrl_reply(220, "FreeShop FTP ready.");
        }
        out->status = FTP_STATUS_LISTENING;
        if (!s_connected) out->client_ip[0] = '\0';
        out->current_file[0] = '\0';
        return;
    }

    out->status = FTP_STATUS_CONNECTED;

    char line[FTP_LINE_MAX];
    bool closed = false;
    if (ctrl_poll_line(line, sizeof(line), &closed)) {
        handle_line(line, out, progress_cb, userdata);
    }

    if (closed || s_close_requested) {
        close_client();
        out->status = FTP_STATUS_LISTENING;
        out->client_ip[0] = '\0';
        out->current_file[0] = '\0';
    }
}
