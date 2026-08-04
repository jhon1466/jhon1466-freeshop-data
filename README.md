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
- [`client/`](client/) - devkitPro/libnx C project (SDL2-rendered, Tinfoil-
  styled list and grid views, cover icons). `.nro` entries are downloaded
  straight into `sdmc:/switch/<id>/`. `.nsp`/`.xci` both install natively -
  the client parses the container (PFS0 for `.nsp`, the nested "secure" HFS0
  partition for `.xci`), streams every NCA straight into NCM content
  storage, commits the content-meta record, imports the ticket if present,
  and pushes the application record, all without leaving the app (see
  [`install_nsp_native.h`](client/source/install/install_nsp_native.h) /
  [`install_xci_native.h`](client/source/install/install_xci_native.h));
  "Instalar vía DBI" (X button) remains as a manual fallback to
  [DBI](https://github.com/rashevskyv/dbi) for both while this is still
  being verified across real hardware. `.zip`-packaged ports (an `.nro` plus
  the data files/subfolders it needs, already folder-structured the way
  they'd sit on an SD card) use `fileType: "port"` - the client extracts
  them straight into `sdmc:/switch/` (not nested under the entry's `id`) so
  the zip's own top-level folder ends up where the port expects it (see
  [`zip_extract.h`](client/source/install/zip_extract.h)/
  [`install_port.h`](client/source/install/install_port.h)). `sha256` is verified
  when the catalog entry provides one, otherwise skipped (see
  [`docs/catalog-schema.md`](docs/catalog-schema.md)). It always fetches
  fresh on launch, so any edit made in `/admin` is picked up the next time
  the client is opened - no extra "update detection" logic needed. An
  always-visible category tab strip sits below the storage panels (built
  from whatever `category` values are in the catalog, no "show everything"
  tab - it always has one selected) with `ZL`/`ZR` button-hint boxes in each
  corner; the active tab is underlined so it's obvious which catalog is
  showing. `R` opens the system keyboard to search by title. A persistent
  sidebar (Tinfoil-style - see
  [`ui_list.c`](client/source/ui/ui_list.c)'s `draw_sidebar`) sits to the
  left of the catalog with every other screen this client has - Explorador,
  Cola, Guardados, Fuentes, Acerca de - listed by name and a small hand-drawn
  icon each (`draw_sidebar_icon` - plain rects/lines, the same toolset
  `draw_queue_badge`'s checkmark already used; no image assets or extra
  libraries) instead of scattered across single-button shortcuts that used
  up every button on the controller. `L` moves input focus into the sidebar
  (`Up`/`Down` to pick a section, `A` to open it, `L` again to come back to
  the catalog); every other button keeps its usual catalog meaning
  regardless of which section is highlighted in the sidebar. `-` collapses
  it to an icon-only rail and back (persisted to `prefs.json`, works from
  either focus state) - the content area reflows into the freed width
  (wider list columns, a more centered grid) rather than leaving it empty,
  and the width itself eases open/closed via `ui_fx_ease` instead of
  snapping, the same easing already driving the grid's selection zoom and
  the list's sliding highlight. The touchscreen works alongside the
  controller on this screen: tapping a sidebar row focuses, selects, and
  opens it in one gesture, tapping a grid/list entry selects and installs it
  the same way, and tapping the sidebar's collapse row toggles it, all via a
  single `hidGetTouchScreenStates()` poll per frame hit-tested against the
  same rects this screen already renders with (see `ui_show_list`) - the
  digitizer matches the display 1:1 at 1280x720, so no coordinate scaling is
  needed. Touch isn't wired up anywhere else yet (Explorador, Cola, Fuentes,
  Guardados, Acerca de, and every confirmation dialog still need the
  controller). Every panel/highlight box on this screen (the sidebar, its
  selection highlight, the storage gauges, the category tab bar, the grid's
  selection box) is drawn with `ui_draw_rounded_rect`
  ([`ui_app.c`](client/source/ui/ui_app.c) - filled via horizontal scanline
  spans from a circle equation, since neither SDL2 nor any linked library
  has a native rounded-rect/circle primitive) instead of the flat
  `ui_draw_rect` the rest of the app still uses. "Acerca de" (app name/version, a short
  blurb, and a donations panel with a PayPal QR + email) is one of those
  sections - the QR is bundled into the `.nro` itself via RomFS
  (`client/romfs/qr.jpg`, mounted at `romfs:/` in `main.c`), so it's shown
  without needing network. View mode, sort
  mode, and the active category filter are saved to
  `sdmc:/switch/freeshop/prefs.json` and restored on the next launch (see
  [`ui_prefs.h`](client/source/ui/ui_prefs.h)). Holding the
  selection still for 1s reveals a title's full, untruncated name (grid
  cells especially cut long titles short). The install screen shows a live
  speed/time-remaining estimate. The client itself also self-updates: on
  launch it checks the data repo's GitHub Releases for a newer version
  (showing that release's own notes before asking for confirmation) and,
  if confirmed, downloads and replaces its own `.nro` - see
  [Publishing a client update](#publishing-a-client-update) below. The
  replace itself is a two-hop chain-load (a running `.nro` can't
  remove()/rename() itself on real hardware): the update downloads to a
  `.update`-suffixed staging copy next to the current file, chain-loads into
  it, and *that* process (now running from a different file) swaps the
  staging copy onto the canonical path and chain-loads back - so from the
  user's side it's just a couple of quick screen flashes, no manual
  close/reopen needed. Chain-loading (`envSetNextLoad`) only works in launch
  environments that support it (plain hbmenu launches do; some NSP-forwarder
  setups don't - checked via `envHasNextLoad()` before relying on it) - if
  unavailable, it falls back to directly overwriting the running file's
  on-disk content in place and asking the user to close and reopen manually,
  same as the original (pre-two-hop) behavior. A save-data manager
  (Guardados, one of the sidebar sections) lists every installed title's
  Account-type save data on the console, regardless of how it was installed -
  not limited to the FreeShop catalog (see
  [`saves/save_scan.c`](client/source/saves/save_scan.c), which
  enumerates `FsSaveDataInfo` via `fsOpenSaveDataInfoReader` and resolves
  each title's name/icon via `nsGetApplicationControlData`). `A` opens a
  title's backups (`Y` there creates a new one with a progress screen of its
  own - large saves no longer make the screen appear to hang, `A` restores
  one after a confirmation, `X` deletes one); `Y` from the title list backs
  up immediately without drilling in. Each backup is a single
  Deflate-compressed `.zip` (via
  [`install/zip_create.c`](client/source/install/zip_create.c), the write
  counterpart to the `.zip`-port installer's own
  [`zip_extract.c`](client/source/install/zip_extract.c) - same on-disk
  struct layout, raw deflate via zlib, no external tool/library beyond
  what's already linked) named `<game name> [<application id hex>]/<profile>/<timestamp>.zip`
  under `sdmc:/switch/freeshop/saves/` - the game's own (sanitized) name
  leads the path specifically so a backup is findable in a file explorer
  without cross-referencing the id against anything else first (see
  `backup_dir_for_entry` in
  [`saves/save_backup.c`](client/source/saves/save_backup.c); backups made
  before this - unzipped folders under an id-only path - aren't picked up
  by the new scheme, migrating them wasn't worth the complexity for a
  feature this new). `fsdevMountSaveData` mounts the live save data for
  both directions; a restore commits the journal (`fsdevCommitDevice`)
  afterwards, required for a save-data write to actually persist, and only
  overwrites files the backup has, never deleting files the live save has
  that the backup doesn't.

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
docker run --rm -v "${PWD}/client:/workspace" freeshop-client-builder make
```

Produces `client/freeshop-client.nro`. It reads the catalog directly from
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
2. Build (`docker build -t freeshop-client-builder client/` once, then
   `docker run --rm -v "$(pwd)/client:/workspace" freeshop-client-builder sh -c "cd /workspace && make"`).
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
