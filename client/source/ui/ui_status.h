#pragma once
#include <switch.h>
#include <stdbool.h>
#include <stddef.h>

typedef struct {
    char clock[12];            // "HH:MM:SS", empty string if unavailable. Sized for
                                // snprintf's worst-case %02u width (hour/minute/second
                                // are u8, so GCC can't assume <100), not just "HH:MM:SS\0".
    bool battery_ok;
    u32 battery_percent;
    bool charging;
    bool network_ok;         // true if the internet connection is up.
    char network_label[16];  // Always populated: "WiFi"/"Ethernet"/"Sin conexión".
} SystemStatus;

// Display-only status query (clock, battery, network). The caller is
// expected to re-call this on a coarse timer (ui_list.c does ~1x/sec) rather
// than every frame, since that would mean opening/closing time/psm/nifm
// sessions 60x/sec for no practical benefit.
void ui_status_refresh(SystemStatus *out);
