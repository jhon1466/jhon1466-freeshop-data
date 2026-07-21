# FreeShop

A homebrew "shop" for the Nintendo Switch: a backend index server serving a
JSON catalog of homebrew apps, and a native Switch client (libnx) that lets
you browse the catalog and install apps directly to the SD card from the
Homebrew Menu.

## Layout

- [`shared/catalog.schema.json`](shared/catalog.schema.json) - JSON Schema
  for the catalog document. See [`docs/catalog-schema.md`](docs/catalog-schema.md).
- [`server/`](server/) - Node.js + Express + TypeScript index server. Serves
  `GET /api/apps`, `GET /api/apps/:id`, and static icons/downloads. No
  database for v1 - `server/data/catalog.json` is hand-edited.
- [`client/`](client/) - devkitPro/libnx C project. Fetches the catalog,
  shows a console-based list/detail UI, downloads and SHA-256-verifies the
  selected `.nro`, and writes it to `sdmc:/switch/<id>/`.

## Quick start

### Backend

```
cd server
npm install
npm run validate-catalog   # sanity-check server/data/catalog.json
npm run dev                # http://localhost:8080
```

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

Produces `client/freeshop-client.nro`. Before running it on a Switch or in
an emulator, edit `client/source/config.h` and set `CATALOG_BASE_URL` to
wherever your server is actually reachable (a LAN IP, e.g.
`http://192.168.1.50:8080`), then rebuild.

A GitHub Actions workflow (`.github/workflows/build-client.yml`) builds the
same way in CI once this repo is pushed to a GitHub remote.

## Adding an app to the catalog

See [`docs/catalog-schema.md`](docs/catalog-schema.md#adding-an-app-v1-no-admin-ui).
`server/data/catalog.json` currently contains one placeholder entry
(`hello-homebrew`) - a plain text file standing in for a real `.nro`, used
to exercise the download + checksum flow end-to-end. Replace it with real
homebrew once you have some to list.

## Status

This is a v1 walking skeleton: one catalog entry, no search/categories
filtering, no update-checking, no auth, no HTTPS, no admin UI. See the
"Follow-ups" section of the original implementation plan for the rest of
the roadmap.
