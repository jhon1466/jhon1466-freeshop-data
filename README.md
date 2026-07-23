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
  from whatever `category` values are in the catalog); `ZL`/`ZR` step
  between tabs directly, with the active one underlined so it's obvious
  which catalog is showing. `R` opens the system keyboard to search by
  title. View mode, sort mode, and the active category filter are
  saved to `sdmc:/switch/freeshop/prefs.json` and restored on the next
  launch (see [`ui_prefs.h`](client/source/ui/ui_prefs.h)). Holding the
  selection still for 1s reveals a title's full, untruncated name (grid
  cells especially cut long titles short). The install screen shows a live
  speed/time-remaining estimate. The client itself also self-updates: on
  launch it checks the data repo's GitHub Releases for a newer version
  (showing that release's own notes before asking for confirmation) and,
  if confirmed, downloads and replaces its own `.nro` - see
  [Publishing a client update](#publishing-a-client-update) below.

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
3. On `jhon1466-freeshop-data`, create a GitHub Release tagged `v<version>`
   (a leading `v` is optional - stripped before comparing) and attach the
   built `client/freeshop-client.nro` as a release asset named **exactly**
   `freeshop-client.nro` (see `CLIENT_RELEASE_ASSET_NAME` in `config.h`).
   Whatever you write as that release's description is shown to users in
   the update confirmation dialog - worth actually filling in.

The version check is a plain string comparison, not semver-aware - it only
detects "different from what's running", so don't publish a release with an
older version string than one already out (clients that already updated
would see it as "available" again).

## Status

This is a v1 walking skeleton: one catalog entry, no search/categories
filtering, no auth on the public API, no HTTPS. See the "Follow-ups" section
of the original implementation plan for the rest of the roadmap.
