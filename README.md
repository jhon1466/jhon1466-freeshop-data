# FreeShop

A homebrew "shop" for the Nintendo Switch: a backend index server serving a
JSON catalog of homebrew apps, and a native Switch client (libnx) that lets
you browse the catalog and install apps directly to the SD card from the
Homebrew Menu.

## Layout

- [`shared/catalog.schema.json`](shared/catalog.schema.json) - JSON Schema
  for the catalog document. See [`docs/catalog-schema.md`](docs/catalog-schema.md).
- [`server/`](server/) - Node.js + Express + TypeScript index server. Serves
  `GET /api/apps`, `GET /api/apps/:id`, static icons/downloads, and the admin
  page at `/admin`. Reads the catalog straight from its GitHub data repo
  (`raw.githubusercontent.com`) on every request - no restart needed after an
  edit, and no database/read-quota to manage. `server/data/catalog.json` is
  now just a historical reference/seed example, no longer read at runtime.
- [`admin/`](admin/) - Build-free static admin page (`/admin`), gated by a
  single shared admin password. Saves commit the whole catalog document to
  the GitHub data repo via the Contents API (server-side, using a token that
  never reaches the browser). See [Admin setup](#admin-setup) below.
- [`functions/`](functions/) - Firebase Cloud Function used for the hosted
  production deployment (wraps the same Express app as `server/`, minus the
  static routes - see [`functions/README.md`](functions/README.md)).
  [`firebase.json`](firebase.json) has Hosting serve `/admin`, `/icons` and
  `/downloads` directly and rewrite `/api/**` to this Function.
- [`worker/`](worker/) - Cloudflare Worker that proxies the Switch client's
  catalog/icon/download reads to the GitHub data repo, so the repo's
  owner/name never shows up in `sources.json`, the compiled `.nro`'s
  strings, or a packet capture - only the Worker's own URL does. See
  [`worker/README.md`](worker/README.md) for deploy steps.
- [`client/`](client/) - devkitPro/libnx C/C++ project. The UI is built on
  [Borealis](https://github.com/XITRIX/borealis) (`moonlight_wiliwili`
  branch - the same fork/branch [pipensx](https://github.com/i3sey/pipensx)
  itself uses, vendored under `client/vendor/borealis/` but not committed,
  see [`CMakeLists.txt`](client/CMakeLists.txt)), themed with  own
  design tokens (see [`views/theme.cpp`](client/source/views/theme.cpp)).
  Everything below the UI - catalog fetching, the torrent engine, NSP/XCI/NSZ
  installers, MTP/FTP, save backups - is plain C, reached from the C++ views
  via `extern "C"` headers; none of that changed in the move to Borealis.
  The app shell ([`views/main_frame.cpp`](client/source/views/main_frame.cpp))
  is a `TabFrame` sidebar with eight sections:
  - **Catalogo** - a 5-column `RecyclerFrame` grid
    ([`views/catalog_tab.cpp`](client/source/views/catalog_tab.cpp)) fed by
    every enabled source ([`views/catalog_service.cpp`](client/source/views/catalog_service.cpp)),
    cover art loaded async and disk-cached
    ([`views/async_cover.cpp`](client/source/views/async_cover.cpp)).
    Selecting a game opens
    [`views/game_detail_activity.cpp`](client/source/views/game_detail_activity.cpp) -
    facts, description, an install button with live progress/cancel, a
    queue toggle, and a DLC/update list where each row reuses the same
    detail screen recursively.
  - **Cola** - the download queue
    ([`views/queue_tab.cpp`](client/source/views/queue_tab.cpp)), installing
    queued titles one at a time with live progress and cancel; the queued-id
    set itself lives in
    [`install/download_queue.c`](client/source/install/download_queue.c).
  - **Explorador** - a plain SD file browser
    ([`views/explorer_tab.cpp`](client/source/views/explorer_tab.cpp)):
    navigate folders, delete files/folders.
  - **Guardados** - every installed title's Account-type save data on the
    console ([`views/saves_tab.cpp`](client/source/views/saves_tab.cpp)),
    each opening a backup/restore/delete screen
    ([`views/save_detail_activity.cpp`](client/source/views/save_detail_activity.cpp))
    backed by [`saves/save_scan.c`](client/source/saves/save_scan.c) /
    [`saves/save_backup.c`](client/source/saves/save_backup.c) - unchanged
    from before the Borealis move.
  - **MTP** / **FTP** - start/stop the PTP responder / FTP server and watch
    live status and transfer history
    ([`views/mtp_tab.cpp`](client/source/views/mtp_tab.cpp),
    [`views/ftp_tab.cpp`](client/source/views/ftp_tab.cpp)), each polling its
    blocking C step function from its own background thread so a transfer
    never freezes the UI.
  - **Fuentes** - toggle, remove, or add a catalog source
    ([`views/sources_tab.cpp`](client/source/views/sources_tab.cpp)), typing
    a new one's URL with the Switch's own software keyboard
    (`brls::Application::getImeManager()`).
  - **Acerca de** - app name/version and attribution
    ([`views/about_tab.cpp`](client/source/views/about_tab.cpp)).

  Known gap from the pre-Borealis client: self-update (checking the data
  repo's GitHub Releases and chain-loading a replacement `.nro` - see
  [Publishing a client update](#publishing-a-client-update) below) hasn't
  been ported to a Borealis screen yet.

## Quick start

### Backend

```
cd server
npm install
cp .env.example .env       # fill in ADMIN_PASSWORD and GITHUB_TOKEN - see Admin setup
npm run validate-catalog   # sanity-check server/data/catalog.json before importing it
npm run dev                # http://localhost:8080
```

If the catalog doesn't exist yet in the data repo (first run) or fails
validation, `npm run dev` logs a warning and keeps running - `/admin` stays
reachable so you can create it (see [Admin setup](#admin-setup)); `/api/apps`
returns a 500 until then.

```
curl http://localhost:8080/api/health
curl http://localhost:8080/api/apps
```

### Client

The client is built inside devkitPro's official Docker image rather than
requiring a native devkitPro install - see
[`docs/build-with-docker.md`](docs/build-with-docker.md) for details
(Docker Desktop must be installed and running):

```
docker build -t freeshop-client-builder client/
git clone --depth 1 --branch moonlight_wiliwili --recurse-submodules \
  --shallow-submodules https://github.com/XITRIX/borealis.git client/vendor/borealis
docker run --rm -v "${PWD}/client:/workspace" freeshop-client-builder \
  sh -c "cmake -S . -B build-cmake -DCMAKE_BUILD_TYPE=Release && cmake --build build-cmake -j4"
```

Produces `client/build-cmake/freeshop-client.nro`. Borealis (see the client
paragraph above) is vendored but not committed - clone it once as shown
above; `client/CMakeLists.txt` fails fast with that same command if it's
missing. It reads the catalog directly from
the GitHub data repo's raw content CDN (`client/source/config.h`'s
`CATALOG_BASE_URL`/`CATALOG_API_PATH`) rather than from the Firebase-hosted
server - real-hardware testing found libnx's network stack couldn't
complete a TLS connection to Google's frontend (Firebase Hosting/Cloud Run)
at all, while `raw.githubusercontent.com` is a well-established working
target for Switch homebrew. Only change these if you're using a different
data repo (see [Admin setup](#admin-setup)), then rebuild.

A GitHub Actions workflow (`.github/workflows/build-client.yml`) builds the
same way in CI once this repo is pushed to a GitHub remote.

## Admin setup

The catalog now lives as a JSON file in a dedicated **public** GitHub data
repo (kept separate from this code repo, e.g. `jhon1466/freeshop-data`),
read via the free `raw.githubusercontent.com` CDN - no database, no
per-read billing/quota. `/admin` edits it by committing through the GitHub
Contents API. One-time setup, all manual (account-level, can't be scripted
from here):

1. Create a new **public** GitHub repo for the data (e.g. `freeshop-data`).
   It can start empty - the first save from `/admin` creates
   `data/catalog.json` with an initial commit.
2. Generate a **fine-grained Personal Access Token**
   (github.com -> Settings -> Developer settings -> Fine-grained tokens) -
   scope it to just that repo, permission **Contents: Read and write**.
3. In `server/.env`, set:
   - `ADMIN_PASSWORD` - whatever password you want to use to log into
     `/admin`.
   - `GITHUB_TOKEN` - the token from step 2.
   - `GITHUB_OWNER` / `GITHUB_REPO` - only if they differ from the defaults
     (`jhon1466` / `jhon1466-freeshop-data`) baked into
     [`server/src/lib/githubConfig.ts`](server/src/lib/githubConfig.ts).

Then, with the server running (see Quick start below), open
`http://localhost:8080/admin`, log in with `ADMIN_PASSWORD`, and use
**Importar JSON** to paste the contents of `server/data/catalog.json` once to
commit the initial `hello-homebrew` entry into the data repo. From then on,
add/edit/delete apps directly in the admin page - each save is a real commit,
and `GET /api/apps` reflects it immediately, no server restart.

## Production deploy (Firebase)

`server/` is for local dev. Production is a Firebase Cloud Function (`api`)
behind Firebase Hosting - see [`functions/README.md`](functions/README.md)
for the one-time setup (Blaze plan, `firebase functions:secrets:set` for
`ADMIN_PASSWORD`/`GITHUB_TOKEN`) and `npm run deploy` from `functions/` to
ship it.

## Adding an app to the catalog

See [`docs/catalog-schema.md`](docs/catalog-schema.md#adding-an-app). Icons
are committed to the GitHub data repo (`icons/<id>.jpg`) rather than uploaded
through the admin page or stored in Firebase Hosting - that keeps them off
Hosting's storage quota entirely, served instead via
`raw.githubusercontent.com`. Downloadable `.nro`/`.nsp`/`.xci` files still go under
`server/public/downloads/` (or the data repo / an external host in
production). Then fill in the entry's fields - including `fileSize` (use
the "Calcular fileSize desde downloadUrl" button, a cheap HEAD request) and
the icon's raw GitHub URL - through `/admin`. `sha256` is optional and
unverified by the Switch client - see
[`docs/catalog-schema.md`](docs/catalog-schema.md) for why.

## Publishing a client update

The client checks jhon1466-freeshop-data's GitHub Releases for a newer
version on every launch (see
[`self_update.h`](client/source/update/self_update.h)) - to ship one:

1. Bump `CLIENT_VERSION` in [`config.h`](client/source/config.h).
2. Build - see [Client](#client) above for the one-time Borealis clone, then
   `docker run --rm -v "$(pwd)/client:/workspace" freeshop-client-builder sh -c "cmake -S . -B build-cmake -DCMAKE_BUILD_TYPE=Release && cmake --build build-cmake -j4"`.
3. Publish it:

   ```
   node scripts/publish-client-release.js v<version> "release notes"
   ```

   That creates the GitHub Release on `jhon1466-freeshop-data` tagged
   `v<version>` and attaches the built `client/freeshop-client.nro` under
   the asset name the updater expects (`CLIENT_RELEASE_ASSET_NAME` in
   `config.h`). It refuses to publish if the tag doesn't match the
   `CLIENT_VERSION` the `.nro` was actually built with, which is the easy
   way to ship an update consoles then re-offer forever. Long notes can go
   in a file instead: `--notes-file notes.txt`.

   The script needs `GITHUB_TOKEN` in `server/.env` (gitignored). Doing
   this by hand through GitHub's web UI works too - the tag's leading `v`
   is optional (stripped before comparing) and only the asset name has to
   be exact.

   Whatever you write as the release's description is shown to users in the
   update confirmation dialog - worth actually filling in.

The version check is a plain string comparison, not semver-aware - it only
detects "different from what's running", so don't publish a release with an
older version string than one already out (clients that already updated
would see it as "available" again).

## Status

This is a v1 walking skeleton: one catalog entry, no search/categories
filtering, no auth on the public API, no HTTPS. See the "Follow-ups" section
of the original implementation plan for the rest of the roadmap.

## License

GNU General Public License v3.0 - see [`LICENSE`](LICENSE). The Switch client
(`client/`) incorporates torrent-download code adapted from
[pipensx](https://github.com/i3sey/pipensx) (GPL-3.0) and embeds
[jech/dht](https://github.com/jech/dht) (MIT) - see
`client/THIRD_PARTY_NOTICES.md` for the full accounting.
