// Republishes the local, Spanish-enriched game_metadata_index.json (built by
// merging scripts/translate-metadata-es.js's output into the bundled
// snapshot) to the "metadata-mirror" release on our own data repo, so
// already-installed clients pick up descriptionEs on their next live sync —
// not just fresh installs via the bundled romfs snapshot.
//
// Preserves the existing manifest's generatedAt/langegenCommit/titledbCommit/
// entries (provenance of the underlying upstream dataset is unchanged - only
// the index content grew a descriptionEs field), recomputing just sha256 and
// bytes to match the new file so GameMetadataService::prepareSnapshot()'s
// checksum validation still passes.
//
// Usage:
//   node scripts/publish-metadata-es.js
//
// Reads GITHUB_TOKEN from server/.env (gitignored). The token is never
// printed, including in error output.
const fs = require("node:fs");
const path = require("node:path");
const crypto = require("node:crypto");

const REPO = "jhon1466/jhon1466-freeshop-data";
const TAG = "metadata-mirror";

const root = path.resolve(__dirname, "..");
const indexPath = path.join(root, "client/resources/catalog/game_metadata_index.json");

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
  const indexBytes = fs.readFileSync(indexPath);
  const sha256 = crypto.createHash("sha256").update(indexBytes).digest("hex");
  const entries = JSON.parse(indexBytes.toString("utf8"));
  console.log(`Local index: ${indexBytes.length} bytes, ${entries.length} entries, sha256 ${sha256}`);

  const headers = {
    Authorization: `Bearer ${readToken()}`,
    Accept: "application/vnd.github+json",
    "X-GitHub-Api-Version": "2022-11-28",
    "User-Agent": "freeshop-metadata-sync-script",
  };

  console.log(`Looking up existing release ${TAG} on ${REPO}`);
  const existing = await githubJson(`https://api.github.com/repos/${REPO}/releases/tags/${TAG}`, {
    headers,
  });
  console.log(`Found ${existing.html_url} (id ${existing.id})`);

  const manifestAsset = existing.assets.find((a) => a.name === "manifest.json");
  if (!manifestAsset) throw new Error("existing release has no manifest.json asset");
  const manifestRes = await fetch(manifestAsset.browser_download_url, { redirect: "follow" });
  if (!manifestRes.ok) throw new Error(`fetch existing manifest -> HTTP ${manifestRes.status}`);
  const manifest = await manifestRes.json();

  const indexAsset = existing.assets.find((a) => a.name === "game_metadata_index.json");
  if (!indexAsset) throw new Error("existing release has no game_metadata_index.json asset");

  const updatedManifest = {
    ...manifest,
    index: {
      ...manifest.index,
      sha256,
      bytes: indexBytes.length,
      entries: entries.length,
      url: indexAsset.browser_download_url,
    },
  };

  for (const asset of [manifestAsset, indexAsset]) {
    console.log(`Deleting old asset ${asset.name} (id ${asset.id})`);
    const deleteRes = await fetch(`https://api.github.com/repos/${REPO}/releases/assets/${asset.id}`, {
      method: "DELETE",
      headers,
    });
    if (!deleteRes.ok && deleteRes.status !== 404) {
      throw new Error(`DELETE asset ${asset.id} -> HTTP ${deleteRes.status}`);
    }
  }

  const uploadUrlBase = existing.upload_url.replace(/\{.*\}$/, "");

  async function uploadAsset(name, bytes) {
    const uploaded = await githubJson(`${uploadUrlBase}?name=${encodeURIComponent(name)}`, {
      method: "POST",
      headers: { ...headers, "Content-Type": "application/octet-stream", "Content-Length": String(bytes.length) },
      body: bytes,
    });
    console.log(`Asset uploaded: ${uploaded.name} (${uploaded.size} bytes)`);
    return uploaded;
  }

  await uploadAsset("game_metadata_index.json", indexBytes);
  await uploadAsset("manifest.json", Buffer.from(JSON.stringify(updatedManifest, null, 2), "utf8"));

  console.log("Done. Clients will pick up descriptionEs on their next metadata refresh.");
}

main().catch((err) => {
  console.error(String(err.message || err));
  process.exit(1);
});
