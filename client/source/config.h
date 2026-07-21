#pragma once

// Base URL of the FreeShop index server. Hardcoded for v1 - a settings
// screen to change this at runtime is a follow-up once the walking-skeleton
// flow works end-to-end.
//
// This is the LAN IP of the dev machine running `npm run dev` in server/
// (Ethernet adapter, DHCP-assigned). If that machine's IP changes - e.g. a
// new DHCP lease, or you run the server elsewhere - update this and rebuild.
#define CATALOG_BASE_URL "http://192.168.100.175:8080"

#define CATALOG_API_PATH "/api/apps"

#define SWITCH_APPS_ROOT "sdmc:/switch"
