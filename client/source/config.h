#pragma once

// Bootstrap default for the "Fuentes" screen (see catalog/sources.h): used
// only the first time the app runs, to seed sdmc:/switch/freeshop/sources.json
// with one usable entry. After that, sources.json is the source of truth -
// users can add/remove/toggle catalog sources on-console (the "-" button
// from the main list), so this value stops mattering once that file exists.
//
// The Switch client reads the catalog straight from the GitHub data repo's
// raw content CDN, NOT from the Firebase-hosted server (/firebase.json,
// /functions) - real-hardware testing found libnx's network stack/mbedtls
// build can't complete a TLS connection to Google's frontend (Firebase
// Hosting/Cloud Run) at all, timing out even with generous timeouts and
// IPv4 forced (see net/http.c). raw.githubusercontent.com is a
// well-established working target for Switch homebrew HTTPS. The admin
// panel (a browser, not this client) still runs on Firebase - only this
// on-console read path is rerouted. A tradeoff: raw.githubusercontent.com
// can lag a live edit by a few minutes (its CDN cache), which is fine for
// browsing but not for the admin's own immediate save feedback (that's why
// the server/Function still reads via GitHub's Contents API instead - see
// server/src/lib/catalog.ts).
//
// TLS certificate verification is disabled in net/http.c (no CA store on
// Switch) - download integrity is still guaranteed independently via the
// sha256 check in install/install.c.
#define CATALOG_BASE_URL "https://raw.githubusercontent.com/jhon1466/jhon1466-freeshop-data/main"

#define CATALOG_API_PATH "/data/catalog.json"

#define SWITCH_APPS_ROOT "sdmc:/switch"

// Bumped by hand with every release. Compared as a plain string (not
// semver-aware) against the latest GitHub Release's tag on
// jhon1466-freeshop-data - see update/self_update.h. To publish a new
// version: bump this, build, tag a GitHub Release on that repo as
// "v<this value>" (a leading "v" is stripped before comparing, either form
// works), and attach the built freeshop-client.nro as a release asset named
// exactly "freeshop-client.nro".
#define CLIENT_VERSION "1.1.1"

// GitHub Releases API endpoint checked for a new client version - see
// update/self_update.c.
#define CLIENT_RELEASES_API_URL "https://api.github.com/repos/jhon1466/jhon1466-freeshop-data/releases/latest"
#define CLIENT_RELEASE_ASSET_NAME "freeshop-client.nro"
