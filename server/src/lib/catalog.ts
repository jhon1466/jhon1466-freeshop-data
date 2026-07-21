import fs from "node:fs";
import path from "node:path";
import { CatalogDocument, AppEntry } from "../types/catalog";
import { validateCatalogDocument } from "./validateCatalog";

const CATALOG_PATH = path.resolve(__dirname, "../../data/catalog.json");

export class CatalogError extends Error {}

// Re-reads and re-validates catalog.json from disk on every call. The file is
// small and hand-edited, so this trades a negligible amount of disk I/O for
// never serving a stale in-memory copy after an edit + restart-less deploy.
export function loadCatalog(): CatalogDocument {
  const raw = fs.readFileSync(CATALOG_PATH, "utf-8");
  const doc = JSON.parse(raw);
  const result = validateCatalogDocument(doc);
  if (!result.valid) {
    const details = (result.errors ?? [])
      .map((e) => `${e.instancePath || "/"} ${e.message}`)
      .join("; ");
    throw new CatalogError(`catalog.json failed schema validation: ${details}`);
  }
  return doc as CatalogDocument;
}

export function findApp(id: string): AppEntry | undefined {
  const catalog = loadCatalog();
  return catalog.apps.find((app) => app.id === id);
}
