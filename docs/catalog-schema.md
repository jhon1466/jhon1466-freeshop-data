# Catalog schema

The catalog is a single JSON document served at `GET /api/apps` and validated
against [`shared/catalog.schema.json`](../shared/catalog.schema.json) (JSON
Schema draft-07). It lives as `data/catalog.json` in a dedicated public
GitHub data repo, edited through the admin page at `/admin` (see the root
[README](../README.md#admin-setup) for one-time setup). The server refuses
to start if that file doesn't exist or doesn't validate.
`server/data/catalog.json` is kept only as a historical/seed example of the
shape - it's no longer read at runtime.

## Document shape

```json
{
  "schemaVersion": 1,
  "generatedAt": "2026-07-20T00:00:00Z",
  "apps": [ /* AppEntry[] */ ]
}
```

`schemaVersion` lets a future client detect an incompatible catalog format
and show a friendly message instead of crashing on unexpected structure.

## AppEntry fields

| Field | Type | Required | Notes |
|---|---|---|---|
| `id` | string | yes | Stable slug (`^[a-z0-9]+(-[a-z0-9]+)*$`). Used as the download/icon folder key and the SD card install folder (`sdmc:/switch/<id>/`). |
| `title` | string | yes | Display name. |
| `author` | string | yes | |
| `category` | string | yes | Free text for v1. |
| `description` | string | yes | Short summary (<=200 chars), shown in the list view. |
| `longDescription` | string | no | Shown on the detail screen. |
| `version` | string | yes | |
| `iconUrl` | string | yes | Absolute URL, e.g. `https://raw.githubusercontent.com/<owner>/<data-repo>/main/icons/<id>.jpg` - icons are committed to the GitHub data repo (not Firebase Hosting) so they don't count against its storage. Recommend ~256x256, <100KB. **Must be JPEG or PNG** - the Switch client (`client/source/ui/ui_icons.c`) only decodes those two formats for the grid view; WEBP is accepted by the admin upload endpoint for browser convenience but won't render on-console. This is the catalog-listing icon, distinct from the client's own NACP launcher icon (`client/icon.jpg`). |
| `downloadUrl` | string | yes | e.g. `/downloads/<id>/<version>/<file>.nro`. Can be same-origin or an external URL. |
| `fileSize` | integer | yes | Bytes. Used for the progress bar and a pre-download free-space check. |
| `sha256` | string | no | 64 lowercase hex chars, if present. **Not verified by the Switch client** - for large (multi-GB) game files, hashing the whole download on both the admin panel (which would have to fetch the entire file just to hash it) and again on-console (slow, no hardware crypto acceleration) was more cost than the corruption protection was worth for this project. Leave it out; if you want the extra check anyway, compute it yourself (`sha256sum`) and paste it in - `install/install.c` and `install/install_nsp.c` still verify it when present. |
| `filename` | string | yes | e.g. `"Moonlight.nro"`, `"SomeForwarder.nsp"`, or `"SomeGame.xci"`. Extension must match `fileType`. |
| `fileType` | string | no | `"nro"` (default), `"nsp"`, or `"xci"`. `"nro"` is downloaded straight into `sdmc:/switch/<id>/` and launched from hbmenu. `"nsp"`/`"xci"` both install natively (NCAs streamed into NCM content storage, ticket imported, application record pushed) - see [install_nsp_native.h](../client/source/install/install_nsp_native.h)/[install_xci_native.h](../client/source/install/install_xci_native.h); a manual "Instalar vía DBI" fallback is also offered for both, via [install_nsp.h](../client/source/install/install_nsp.h)'s DBI hand-off. |
| `homepageUrl` | string | no | |
| `license` | string | no | |
| `parentId` | string | no | If set, this entry is DLC/an update *for* the base game with this id. Hidden from the main list/grid on the Switch client - shown instead in that game's detail screen ("DLC y actualizaciones" section, `Y` to browse, `A` to install one). The target `id` must exist and must not itself have a `parentId` (one level of nesting only) - the admin page enforces both. |
| `contentType` | string | no | `"dlc"` or `"update"`. Only meaningful when `parentId` is set - just the label shown next to this entry in its parent's list (defaults to a generic "DLC" tag if omitted). |
| `source` | object | no | Reserved for future catalog aggregation: `{"origin":"curated"}` today; `{"origin":"aggregated","sourceIndex":"...","sourceId":"..."}` later. Not read by v1 logic. |
| `updatedAt` | string | no | ISO 8601. |

Clients should tolerate unknown/missing-optional fields rather than
rejecting the whole document.

## Adding an app

1. Drop the downloadable file into
   `server/public/downloads/<id>/<version>/<filename>` (production: commit it
   under the same path in the GitHub data repo, or host it elsewhere and use
   an external `downloadUrl`).
2. Commit the listing icon to `icons/<id>.jpg` in the GitHub data repo (e.g.
   via the Contents API or GitHub's web UI), and use its
   `raw.githubusercontent.com` URL as `iconUrl`.
3. Log into `/admin` and add an entry. Use "Calcular fileSize desde
   downloadUrl" to fill in `fileSize` automatically (a cheap HEAD request,
   instant regardless of file size) - `sha256` is optional, leave it blank.
4. `GET /api/apps` reflects the new entry immediately, no server restart.

## Adding an NSP or XCI entry

Same as above, plus set `"fileType": "nsp"` or `"fileType": "xci"` and give
`filename` a matching `.nsp`/`.xci` extension.

Both `"nsp"` and `"xci"` install natively, in-app: the client downloads the
file into `sdmc:/switch/DBI/nsp-repo/<filename>` (shared with DBI's own
folder - see below), locates the actual title data (a `.nsp` is a PFS0
container directly; a `.xci` is a nested HFS0 - root partition table ->
"secure" partition -> the same kind of file entries a PFS0 has, see
[xci_container.h](../client/source/install/xci_container.h)), streams every
referenced NCA into NCM content storage, commits the content-meta record,
imports the ticket/cert if present, and pushes the application record so the
title shows on hbmenu. The detail screen also offers "Instalar vía DBI" (X
button) as a manual fallback for both formats - it uses the same downloaded
file and chain-loads into `sdmc:/switch/DBI/dbi.nro`, so the user needs
[DBI](https://github.com/rashevskyv/dbi) installed at that path. If DBI is
missing, the client shows a message instead of chain-loading.

## Adding DLC or an update

1. Add the DLC/update itself as a normal catalog entry (same steps as
   above - it's just another `.nsp`/`.xci` file with its own `id`,
   `downloadUrl`, etc.).
2. Set its `parentId` to the base game's `id`, and `contentType` to `"dlc"`
   or `"update"`. In `/admin`, the "parentId" field is a dropdown of
   existing base games - pick one instead of typing the id by hand.
3. It disappears from the main catalog list/grid and instead shows up under
   the base game's detail screen.

An entry with a `parentId` can't itself be a parent (no nested DLC-of-DLC) -
the admin page rejects that on save.
