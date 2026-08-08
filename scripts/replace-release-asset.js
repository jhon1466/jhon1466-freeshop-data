// One-off helper: replaces an existing release's freeshop-client.nro (+
// .sha256) with a freshly built copy, without touching the release's tag
// or notes. GitHub won't let you re-upload an asset with the same name, so
// this deletes the old ones first.
//
// Usage:
//   node scripts/replace-release-asset.js v2.0.0
const fs = require("node:fs");
const path = require("node:path");
const crypto = require("node:crypto");

const REPO = "jhon1466/jhon1466-freeshop-data";
const ASSET_NAME = "freeshop-client.nro";
const root = path.resolve(__dirname, "..");
const nroPath = path.join(root, "client", "build-switch", ASSET_NAME);

function readToken() {
  const envPath = path.join(root, "server", ".env");
  const text = fs.readFileSync(envPath, "utf8");
  for (const line of text.split(/\r?\n/)) {
    const match = line.match(/^\s*GITHUB_TOKEN\s*=\s*(.*?)\s*$/);
    if (match) return match[1].replace(/^["']|["']$/g, "");
  }
  throw new Error(`GITHUB_TOKEN not found in ${path.relative(root, envPath)}`);
}

async function githubJson(url, options) {
  const res = await fetch(url, options);
  const body = await res.text();
  if (!res.ok) throw new Error(`${options.method} ${url} -> HTTP ${res.status}\n${body}`);
  return body ? JSON.parse(body) : null;
}

async function uploadAsset(uploadUrlBase, headers, name, bytes) {
  const uploadUrl = `${uploadUrlBase}?name=${encodeURIComponent(name)}`;
  const uploaded = await githubJson(uploadUrl, {
    method: "POST",
    headers: { ...headers, "Content-Type": "application/octet-stream", "Content-Length": String(bytes.length) },
    body: bytes,
  });
  console.log(`Asset uploaded: ${uploaded.name} (${uploaded.size} bytes)`);
  return uploaded;
}

async function main() {
  const [tag] = process.argv.slice(2);
  if (!tag) {
    console.error("usage: node scripts/replace-release-asset.js <vX.Y.Z>");
    process.exit(1);
  }
  if (!fs.existsSync(nroPath)) throw new Error(`${path.relative(root, nroPath)} not found - build the client first`);
  const asset = fs.readFileSync(nroPath);
  const checksum = crypto.createHash("sha256").update(asset).digest("hex");

  const headers = {
    Authorization: `Bearer ${readToken()}`,
    Accept: "application/vnd.github+json",
    "X-GitHub-Api-Version": "2022-11-28",
    "User-Agent": "freeshop-release-script",
  };

  const release = await githubJson(`https://api.github.com/repos/${REPO}/releases/tags/${tag}`, { method: "GET", headers });

  for (const a of release.assets) {
    if (a.name === ASSET_NAME || a.name === `${ASSET_NAME}.sha256`) {
      await githubJson(`https://api.github.com/repos/${REPO}/releases/assets/${a.id}`, { method: "DELETE", headers });
      console.log(`Deleted old asset: ${a.name}`);
    }
  }

  const uploadUrlBase = release.upload_url.replace(/\{.*\}$/, "");
  const uploaded = await uploadAsset(uploadUrlBase, headers, ASSET_NAME, asset);
  await uploadAsset(uploadUrlBase, headers, `${ASSET_NAME}.sha256`, Buffer.from(checksum, "utf8"));
  console.log(`Download URL: ${uploaded.browser_download_url}`);
}

main().catch((err) => {
  console.error(String(err.message || err));
  process.exit(1);
});
