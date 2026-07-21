# Catalog schema

The catalog is a single JSON document served at `GET /api/apps` and validated
against [`shared/catalog.schema.json`](../shared/catalog.schema.json) (JSON
Schema draft-07). The server refuses to start if `server/data/catalog.json`
doesn't validate.

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
| `iconUrl` | string | yes | e.g. `/icons/<id>.jpg`. Recommend ~256x256 JPEG, <100KB. This is the catalog-listing icon, distinct from the client's own NACP launcher icon (`client/icon.jpg`). |
| `downloadUrl` | string | yes | e.g. `/downloads/<id>/<version>/<file>.nro`. Can be same-origin or an external URL. |
| `fileSize` | integer | yes | Bytes. Used for the progress bar and a pre-download free-space check. |
| `sha256` | string | yes | 64 lowercase hex chars. Verified after download, before the file is moved into place. |
| `nroFilename` | string | yes | e.g. `"Moonlight.nro"`. |
| `homepageUrl` | string | no | |
| `license` | string | no | |
| `source` | object | no | Reserved for future catalog aggregation: `{"origin":"curated"}` today; `{"origin":"aggregated","sourceIndex":"...","sourceId":"..."}` later. Not read by v1 logic. |
| `updatedAt` | string | no | ISO 8601. |

Clients should tolerate unknown/missing-optional fields rather than
rejecting the whole document.

## Adding an app (v1, no admin UI)

1. Drop the `.nro` into `server/public/downloads/<id>/<version>/<nroFilename>`.
2. Drop the listing icon into `server/public/icons/<id>.jpg`.
3. Add an entry to `server/data/catalog.json` with the matching `fileSize`
   and `sha256` (`sha256sum path/to/file.nro`).
4. Run `npm run validate-catalog` from `server/` before restarting the
   server.
