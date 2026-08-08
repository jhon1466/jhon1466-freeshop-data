#include "app/update_transaction.h"

#include <switch.h>

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

/* Fallback only - assumes the app lives in a freeshop-client/ subfolder,
 * which not every install does (some sit straight in sdmc:/switch/). The
 * real paths below are derived from argv[0] at runtime instead: this
 * helper always sits right beside the main .nro (see UpdateService's
 * helperPath_ in update_service.cpp), so its own launch directory plus the
 * fixed "freeshop-client.nro" filename gives the exact same target
 * UpdateService staged the update under - same fix as main_switch.cpp's
 * own argv[0]-derived updateTargetPath, just missed here originally. */
static const char *Target = "sdmc:/switch/freeshop-client/freeshop-client.nro";
static const char *Staged = "sdmc:/switch/freeshop-client/freeshop-client.nro.update";
static const char *Marker = "sdmc:/switch/freeshop-client/freeshop-client.nro.update.sha256";
static const char *Backup = "sdmc:/switch/freeshop-client/freeshop-client.nro.previous";
static const char *LogPath = "sdmc:/switch/freeshop-client/freeshop-client-update.log";

static void update_log(const char *format, ...) {
    FILE *file = fopen(LogPath, "ab");
    if (!file)
        return;
    va_list args;
    va_start(args, format);
    vfprintf(file, format, args);
    va_end(args);
    fflush(file);
    fclose(file);
}

int main(int argc, char **argv) {
    /* argv[0] is this helper's own resolved launch path (envSetNextLoad's
     * argv string put it there - same convention main_switch.cpp's own
     * legacy-update-hop code relies on argv[0] for). This helper is always
     * staged right next to the main .nro, so its directory plus the fixed
     * "freeshop-client.nro" filename reconstructs the exact target
     * UpdateService used, whatever folder this particular install actually
     * lives in - falls back to the hardcoded default above only if argv[0]
     * is missing or implausibly short. */
    static char targetBuf[512];
    static char stagedBuf[520];
    static char markerBuf[532];
    static char backupBuf[524];
    const char *target = Target;
    const char *staged = Staged;
    const char *marker = Marker;
    const char *backup = Backup;
    if (argc > 0 && argv[0] && argv[0][0]) {
        const char *slash = strrchr(argv[0], '/');
        size_t dir_len = slash ? (size_t)(slash - argv[0] + 1) : 0;
        if (dir_len + sizeof("freeshop-client.nro") < sizeof(targetBuf)) {
            memcpy(targetBuf, argv[0], dir_len);
            snprintf(targetBuf + dir_len, sizeof(targetBuf) - dir_len,
                     "freeshop-client.nro");
            snprintf(stagedBuf, sizeof(stagedBuf), "%s.update", targetBuf);
            snprintf(markerBuf, sizeof(markerBuf), "%s.sha256", stagedBuf);
            snprintf(backupBuf, sizeof(backupBuf), "%s.previous", targetBuf);
            target = targetBuf;
            staged = stagedBuf;
            marker = markerBuf;
            backup = backupBuf;
        }
    }
    update_paths_t paths = {target, staged, marker, backup};
    char error[256] = {0};
    const bool requested = argc > 1 && argv[1] &&
                           strcmp(argv[1], "--finish-update") == 0;
    update_log("[helper] started requested=%d nextload=%d target=%s\n",
               requested ? 1 : 0, envHasNextLoad() ? 1 : 0, target);
    /* Never re-launch pipensx. Relaunching the full app inside the same
     * hbloader session re-initializes the graphics/applet/service stack a
     * second time and crashes (black screen then fatal). Every path below
     * returns with no nextload set, so the loader drops to HOME and the user
     * relaunches a fresh process manually. */
    if (!requested)
        return 0;
    if (!update_transaction_apply(&paths, error, sizeof(error))) {
        update_log("[helper] apply failed: %s\n", error);
        return 0;
    }
    Result commit = fsdevCommitDevice("sdmc");
    if (R_FAILED(commit)) {
        update_log("[helper] commit failed result=0x%08x\n", commit);
        error[0] = '\0';
        if (!update_transaction_rollback(&paths, error, sizeof(error)))
            update_log("[helper] rollback failed: %s\n", error);
        fsdevCommitDevice("sdmc");
        return 0;
    }
    update_log("[helper] swap committed; returning to HOME\n");
    return 0;
}
