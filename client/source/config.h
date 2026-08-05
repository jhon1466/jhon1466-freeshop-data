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
//
// This URL ends up readable in plain text in sdmc:/switch/freeshop/sources.json
// on every install (see catalog/sources.c) - routed through a Cloudflare
// Worker (see ../../worker/README.md) instead of straight to
// raw.githubusercontent.com so the data repo's owner/name isn't exposed
// there, in the compiled .nro's strings, or on the wire. The Worker also
// rewrites any absolute raw.githubusercontent.com URLs it finds inside
// catalog.json (e.g. iconUrl - see "Adding an app to the catalog" in the
// main README) to point back at itself, since the client uses those as-is
// (install_common_resolve_url in install_common.c) instead of resolving
// them against this base URL.
#define CATALOG_BASE_URL "https://freeshop-proxy.freeshopnx.workers.dev"

// The pre-Worker default, kept only so sources_load can detect and migrate
// installs whose sources.json still has this baked in as the (hidden)
// bootstrap entry from before CATALOG_BASE_URL pointed at the Worker - see
// the migration in catalog/sources.c.
#define LEGACY_CATALOG_BASE_URL "https://raw.githubusercontent.com/jhon1466/jhon1466-freeshop-data/main"

#define CATALOG_API_PATH "/data/catalog.json"

#define SWITCH_APPS_ROOT "sdmc:/switch"

// Bumped by hand with every release. Compared as a plain string (not
// semver-aware) against the latest GitHub Release's tag on
// jhon1466-freeshop-data - see update/self_update.h. To publish a new
// version: bump this, build, tag a GitHub Release on that repo as
// "v<this value>" (a leading "v" is stripped before comparing, either form
// works), and attach the built freeshop-client.nro as a release asset named
// exactly "freeshop-client.nro".
#define CLIENT_VERSION "1.6.3"

// GitHub Releases API endpoint checked for a new client version - see
// update/self_update.c.
#define CLIENT_RELEASES_API_URL "https://api.github.com/repos/jhon1466/jhon1466-freeshop-data/releases/latest"
#define CLIENT_RELEASE_ASSET_NAME "freeshop-client.nro"
