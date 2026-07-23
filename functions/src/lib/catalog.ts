import { CatalogDocument, AppEntry } from "../types/catalog";
import { validateCatalogDocument } from "./validateCatalog";
import { CATALOG_CONTENTS_API_URL, GITHUB_BRANCH, GITHUB_TOKEN } from "./githubConfig";

export class CatalogError extends Error {}

// raw.githubusercontent.com is a CDN that can lag behind a fresh commit by
// several minutes and doesn't honor cache-busting query params - verified
// this empirically (a save was visible instantly via the Contents API but
// stale on raw for a while after). So reads go through the authenticated
// Contents API instead (always current), with our own short-TTL cache here
// to keep GitHub API call volume low regardless of how many clients hit
// /api/apps - this bounds staleness to CACHE_TTL_MS instead of GitHub's
// opaque CDN behavior.
const CACHE_TTL_MS = 30_000;
let cache: { data: CatalogDocument; fetchedAt: number } | null = null;

async function fetchCatalogFromGithub(): Promise<CatalogDocument> {
  let res: Response;
  try {
    res = await fetch(`${CATALOG_CONTENTS_API_URL}?ref=${GITHUB_BRANCH}`, {
      headers: {
        ...(GITHUB_TOKEN ? { Authorization: `Bearer ${GITHUB_TOKEN}` } : {}),
        Accept: "application/vnd.github.raw",
      },
    });
  } catch (err) {
    throw new CatalogError(
      `Failed to reach GitHub for the catalog: ${err instanceof Error ? err.message : err}`
    );
  }

  if (res.status === 404) {
    throw new CatalogError(
      "catalog.json does not exist yet in the data repo. Log into /admin and save a catalog to create it."
    );
  }
  if (!res.ok) {
    throw new CatalogError(`Failed to fetch catalog from GitHub: HTTP ${res.status}`);
  }

  let data: unknown;
  try {
    data = await res.json();
  } catch (err) {
    throw new CatalogError(`Catalog fetched from GitHub is not valid JSON: ${err}`);
  }

  const result = validateCatalogDocument(data);
  if (!result.valid) {
    const details = (result.errors ?? [])
      .map((e) => `${e.instancePath || "/"} ${e.message}`)
      .join("; ");
    throw new CatalogError(`Catalog from GitHub failed schema validation: ${details}`);
  }
  return data as CatalogDocument;
}

export async function loadCatalog(): Promise<CatalogDocument> {
  if (cache && Date.now() - cache.fetchedAt < CACHE_TTL_MS) {
    return cache.data;
  }
  const data = await fetchCatalogFromGithub();
  cache = { data, fetchedAt: Date.now() };
  return data;
}

// Lets the admin save endpoint make its own follow-up read see the change
// immediately instead of waiting out the rest of the TTL.
export function invalidateCatalogCache(): void {
  cache = null;
}

export async function findApp(id: string): Promise<AppEntry | undefined> {
  const catalog = await loadCatalog();
  return catalog.apps.find((app) => app.id === id);
}
