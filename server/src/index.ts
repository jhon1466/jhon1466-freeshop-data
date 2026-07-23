import "dotenv/config";
import { createApp } from "./app";
import { loadCatalog } from "./lib/catalog";

async function main() {
  if (!process.env.ADMIN_PASSWORD) {
    console.warn(
      "ADMIN_PASSWORD is not set - /admin will be reachable but all saves will be rejected (401)."
    );
  }

  // Warn (don't exit) if the catalog in the GitHub data repo is missing or
  // invalid: unlike v1's hand-edited local file, the very first deployment
  // has no catalog yet, and /admin - which is how one gets created - must
  // stay reachable for that bootstrap to be possible. /api/apps surfaces the
  // same error per-request via CatalogError instead.
  try {
    const catalog = await loadCatalog();
    console.log(`Catalog OK: ${catalog.apps.length} app(s) loaded.`);
  } catch (err) {
    console.warn("Catalog is not available yet (log into /admin to create it):");
    console.warn(err instanceof Error ? err.message : err);
  }

  const app = createApp();
  const PORT = Number(process.env.PORT) || 8080;

  app.listen(PORT, () => {
    console.log(`FreeShop server listening on http://0.0.0.0:${PORT}`);
  });
}

main();
