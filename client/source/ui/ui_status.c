#include "ui_status.h"

#include <stdio.h>
#include <string.h>

void ui_status_refresh(SystemStatus *out) {
    out->clock[0] = '\0';
    out->battery_ok = false;
    out->battery_percent = 0;
    out->charging = false;
    out->network_ok = false;
    out->is_wifi = false;
    out->wifi_strength = 0;
    snprintf(out->network_label, sizeof(out->network_label), "Sin conexión");

    Result rc = timeInitialize();
    if (R_SUCCEEDED(rc)) {
        u64 posix_time = 0;
        if (R_SUCCEEDED(timeGetCurrentTime(TimeType_UserSystemClock, &posix_time))) {
            TimeCalendarTime cal;
            TimeCalendarAdditionalInfo info;
            if (R_SUCCEEDED(timeToCalendarTimeWithMyRule(posix_time, &cal, &info))) {
                snprintf(out->clock, sizeof(out->clock), "%02u:%02u:%02u", cal.hour, cal.minute, cal.second);
            }
        }
        timeExit();
    }

    rc = psmInitialize();
    if (R_SUCCEEDED(rc)) {
        u32 percent = 0;
        if (R_SUCCEEDED(psmGetBatteryChargePercentage(&percent))) {
            out->battery_ok = true;
            out->battery_percent = percent;
        }
        PsmChargerType charger = PsmChargerType_Unconnected;
        if (R_SUCCEEDED(psmGetChargerType(&charger))) {
            out->charging = (charger != PsmChargerType_Unconnected);
        }
        psmExit();
    }

    rc = nifmInitialize(NifmServiceType_User);
    if (R_SUCCEEDED(rc)) {
        NifmInternetConnectionType conn_type = NifmInternetConnectionType_WiFi;
        u32 wifi_strength = 0;
        NifmInternetConnectionStatus conn_status = NifmInternetConnectionStatus_ConnectingUnknown1;
        if (R_SUCCEEDED(nifmGetInternetConnectionStatus(&conn_type, &wifi_strength, &conn_status))) {
            out->network_ok = (conn_status == NifmInternetConnectionStatus_Connected);
            if (out->network_ok) {
                out->is_wifi = conn_type != NifmInternetConnectionType_Ethernet;
                out->wifi_strength = wifi_strength;
                snprintf(out->network_label, sizeof(out->network_label),
                         "%s", out->is_wifi ? "WiFi" : "Ethernet");
            }
        }
        nifmExit();
    }
}
