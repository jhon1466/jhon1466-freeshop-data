// Mirrors i3sey/pipensx-metadata's latest release (manifest.json + the game
// metadata index it references) into our own data repo, so the client's
// optional metadata enrichment (screenshots, developer/publisher, etc. -
// see client/src/app/game_metadata_service.cpp) doesn't depend on upstream's
// GitHub repo staying up. Re-run this periodically to pick up upstream's
// refreshed dataset; the client always points at "latest", so nothing else
// needs to change afterwards.
//
// The mirrored manifest.json is byte-identical to upstream's except for the
// "index" object's "url" field, which is rewritten to point at our own
// re-uploaded copy of game_metadata_index.json - the schemaVersion,
// langegenCommit, titledbCommit, sha256, bytes, and entries fields (and the
// index content itself) are untouched, so
// GameMetadataService::prepareSnapshot()'s checksum validation still passes.
//
// Usage:
//   node scripts/sync-game-metadata.js
//
// Reads GITHUB_TOKEN from server/.env (gitignored). The token is never
// printed, including in error output.
const fs = require("node:fs");
const path = require("node:path");

const REPO = "jhon1466/jhon1466-freeshop-data";
const UPSTREAM_MANIFEST_URL =
  "https://github.com/i3sey/pipensx-metadata/releases/latest/download/manifest.json";

const root = path.resolve(__dirname, "..");

function readToken() {
  const envPath = path.join(root, "server", ".env");
  let text;
  try {
    text = fs.readFileSync(envPath, "utf8");
  } catch {
    throw new Error(`could not read ${path.relative(root, envPath)} - it holds GITHUB_TOKEN`);
  }
  for (const line of text.split(/\r?\n/)) {
    const match = line.match(/^\s*GITHUB_TOKEN\s*=\s*(.*?)\s*$/);
    if (match) return match[1].replace(/^["']|["']$/g, "");
  }
  throw new Error(`GITHUB_TOKEN not found in ${path.relative(root, envPath)}`);
}

async function githubJson(url, options) {
  const res = await fetch(url, options);
  const body = await res.text();
  if (!res.ok) {
    throw new Error(`${options.method || "GET"} ${url} -> HTTP ${res.status}\n${body}`);
  }
  return JSON.parse(body);
}

async function main() {
  console.log(`Fetching ${UPSTREAM_MANIFEST_URL}`);
  const manifestRes = await fetch(UPSTREAM_MANIFEST_URL, { redirect: "follow" });
  if (!manifestRes.ok) throw new Error(`fetch manifest -> HTTP ${manifestRes.status}`);
  const manifest = await manifestRes.json();

  if (manifest.schemaVersion !== 1 || !manifest.index || !manifest.index.url) {
    throw new Error("upstream manifest has an unexpected shape");
  }

  console.log(`Fetching ${manifest.index.url}`);
  const indexRes = await fetch(manifest.index.url, { redirect: "follow" });
  if (!indexRes.ok) throw new Error(`fetch index -> HTTP ${indexRes.status}`);
  const indexBytes = Buffer.from(await indexRes.arrayBuffer());
  if (indexBytes.length !== manifest.index.bytes) {
    throw new Error(
      `downloaded index is ${indexBytes.length} bytes, manifest declares ${manifest.index.bytes}`
    );
  }

  const headers = {
    Authorization: `Bearer ${readToken()}`,
    Accept: "application/vnd.github+json",
    "X-GitHub-Api-Version": "2022-11-28",
    "User-Agent": "freeshop-metadata-sync-script",
  };

  const tag = `metadata-${manifest.langegenCommit.slice(0, 12)}-${manifest.titledbCommit.slice(0, 12)}`;
  console.log(`Creating release ${tag} on ${REPO}`);
  const release = await githubJson(`https://api.github.com/repos/${REPO}/releases`, {
    method: "POST",
    headers: { ...headers, "Content-Type": "application/json" },
    body: JSON.stringify({
      tag_name: tag,
      name: `Metadata mirror ${manifest.generatedAt}`,
      body: `Mirrored from i3sey/pipensx-metadata (generatedAt ${manifest.generatedAt}, ${manifest.index.entries} entries).`,
      draft: false,
      prerelease: false,
    }),
  });
  console.log(`Release created: ${release.html_url}`);

  const uploadUrlBase = release.upload_url.replace(/\{.*\}$/, "");

  async function uploadAsset(name, bytes) {
    const uploaded = await githubJson(`${uploadUrlBase}?name=${encodeURIComponent(name)}`, {
      method: "POST",
      headers: { ...headers, "Content-Type": "application/octet-stream", "Content-Length": String(bytes.length) },
      body: bytes,
    });
    console.log(`Asset uploaded: ${uploaded.name} (${uploaded.size} bytes)`);
    return uploaded;
  }

  const uploadedIndex = await uploadAsset("game_metadata_index.json", indexBytes);

  const mirroredManifest = {
    ...manifest,
    index: { ...manifest.index, url: uploadedIndex.browser_download_url },
  };
  await uploadAsset("manifest.json", Buffer.from(JSON.stringify(mirroredManifest, null, 2), "utf8"));

  console.log("Done. Client points at releases/latest/download, so no further changes are needed.");
}

main().catch((err) => {
  console.error(String(err.message || err));
  process.exit(1);
});
