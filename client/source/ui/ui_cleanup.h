#pragma once

// Shows a small screen with three cleanup actions - each one scans first,
// shows exactly what it found (count + total size), and only deletes after
// an explicit confirm:
//  - stray .part/.hdr temp files anywhere under sdmc:/switch (left behind
//    if the app was force-closed mid-download/install, before its own
//    cleanup could run)
//  - the local icon cache (sdmc:/switch/freeshop/icon_cache)
//  - NCM content orphaned by installs that failed/were interrupted before
//    the automatic rollback in install_nsp_native.c/install_xci_native.c/
//    install_local.c existed - see install/ncm_cleanup.h
// Blocks until the user backs out (B/+).
void ui_show_cleanup(void);
