#include "save_scan.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

// Reused across every nsGetApplicationControlData call in this file - one
// call at a time is ever in flight (saves_scan is sequential, and the icon
// fetch is a separate, later call), so a single static buffer avoids
// reserving ~132KB (sizeof(NacpStruct) + 0x20000 icon) per call site or on
// the stack. Same reasoning as install_common_scratch().
static NsApplicationControlData s_ctrl;

static int compare_save_entry(const void *a, const void *b) {
    const SaveEntry *ea = (const SaveEntry *)a;
    const SaveEntry *eb = (const SaveEntry *)b;
    return strcasecmp(ea->name, eb->name);
}

// Best-effort: resolves the game title into `out_name`, falling back to the
// application id in hex if the control data can't be read.
static void resolve_title(u64 application_id, char *out_name, size_t out_name_size) {
    u64 actual_size = 0;
    Result rc = nsGetApplicationControlData(NsApplicationControlSource_Storage, application_id,
                                             &s_ctrl, sizeof(s_ctrl), &actual_size);
    if (R_SUCCEEDED(rc)) {
        NacpLanguageEntry *lang = NULL;
        nacpGetLanguageEntry(&s_ctrl.nacp, &lang);
        if (lang && lang->name[0] != '\0') {
            snprintf(out_name, out_name_size, "%s", lang->name);
            return;
        }
    }
    snprintf(out_name, out_name_size, "%016llx", (unsigned long long)application_id);
}

int saves_scan(SaveEntry *out, int max, char *err_buf, size_t err_buf_size) {
    if (err_buf && err_buf_size > 0) err_buf[0] = '\0';

    // ---- Account nicknames, resolved once up front so each save's owning
    // profile can be labeled without re-opening acc per entry. ----
    AccountUid uids[ACC_USER_LIST_SIZE];
    char nicknames[ACC_USER_LIST_SIZE][SAVE_ENTRY_NICKNAME_MAX];
    s32 profile_count = 0;

    if (R_SUCCEEDED(accountInitialize(AccountServiceType_Application))) {
        accountListAllUsers(uids, ACC_USER_LIST_SIZE, &profile_count);
        for (s32 i = 0; i < profile_count; i++) {
            nicknames[i][0] = '\0';
            AccountProfile profile;
            if (R_SUCCEEDED(accountGetProfile(&profile, uids[i]))) {
                AccountProfileBase base;
                if (R_SUCCEEDED(accountProfileGet(&profile, NULL, &base))) {
                    snprintf(nicknames[i], sizeof(nicknames[i]), "%s", base.nickname);
                }
                accountProfileClose(&profile);
            }
        }
        accountExit();
    }
    // Failure just means every entry's nickname stays empty (profile_count
    // is left at 0) - not fatal, the save list itself doesn't depend on it.

    Result rc = nsInitialize();
    if (R_FAILED(rc)) {
        if (err_buf) snprintf(err_buf, err_buf_size, "no se pudo conectar con el servicio ns (0x%x)", rc);
        return 0;
    }

    FsSaveDataInfoReader reader;
    rc = fsOpenSaveDataInfoReader(&reader, FsSaveDataSpaceId_User);
    if (R_FAILED(rc)) {
        if (err_buf) snprintf(err_buf, err_buf_size, "no se pudo leer la lista de guardados (0x%x)", rc);
        nsExit();
        return 0;
    }

    int count = 0;
    FsSaveDataInfo info;
    s64 total_read = 0;
    while (count < max && R_SUCCEEDED(fsSaveDataInfoReaderRead(&reader, &info, 1, &total_read))
           && total_read == 1) {
        // Per-profile game saves only - system/bcat/device/cache/temporary
        // saves aren't something a user manages from a game-save backup
        // screen (and several are unsafe to touch casually).
        if (info.save_data_type != FsSaveDataType_Account) continue;

        SaveEntry *e = &out[count];
        e->application_id = info.application_id;
        e->uid = info.uid;
        e->save_data_id = info.save_data_id;
        e->size = (s64)info.size;

        e->nickname[0] = '\0';
        for (s32 i = 0; i < profile_count; i++) {
            if (uids[i].uid[0] == info.uid.uid[0] && uids[i].uid[1] == info.uid.uid[1]) {
                snprintf(e->nickname, sizeof(e->nickname), "%s", nicknames[i]);
                break;
            }
        }

        resolve_title(info.application_id, e->name, sizeof(e->name));
        count++;
    }

    fsSaveDataInfoReaderClose(&reader);
    nsExit();

    qsort(out, count, sizeof(SaveEntry), compare_save_entry);
    return count;
}

bool saves_fetch_icon_jpeg(u64 application_id, unsigned char *out, size_t out_size, size_t *out_len) {
    Result rc = nsInitialize();
    if (R_FAILED(rc)) return false;

    u64 actual_size = 0;
    rc = nsGetApplicationControlData(NsApplicationControlSource_Storage, application_id,
                                      &s_ctrl, sizeof(s_ctrl), &actual_size);
    nsExit();
    if (R_FAILED(rc)) return false;

    size_t icon_len = sizeof(s_ctrl.icon);
    if (actual_size > sizeof(s_ctrl.nacp)) {
        size_t reported = (size_t)(actual_size - sizeof(s_ctrl.nacp));
        if (reported > 0 && reported <= sizeof(s_ctrl.icon)) icon_len = reported;
    }
    if (icon_len > out_size) icon_len = out_size;
    if (icon_len == 0) return false;

    memcpy(out, s_ctrl.icon, icon_len);
    *out_len = icon_len;
    return true;
}
