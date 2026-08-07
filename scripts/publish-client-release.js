// Publishes a GitHub Release on the catalog data repo with the built client
// .nro and its sha256 checksum attached. The client's self-updater
// (client/src/app/update_service.cpp) looks for exactly this shape - tag
// "v<version>" and assets named "freeshop-client.nro" and
// "freeshop-client.nro.sha256" - so the tag must match the version the .nro
// was actually built with (client/VERSION) or consoles will keep
// re-offering the update, and the checksum must be present or
// UpdateService::install() refuses to stage the download.
//
// Usage:
//   node scripts/publish-client-release.js v1.4.4 "release notes"
//   node scripts/publish-client-release.js v1.4.4 --notes-file notes.txt
//
// Reads GITHUB_TOKEN from server/.env (gitignored). The token is never
// printed, including in error output.
const fs = require("node:fs");
const path = require("node:path");
const crypto = require("node:crypto");

const REPO = "jhon1466/jhon1466-freeshop-data";
const ASSET_NAME = "freeshop-client.nro";

const root = path.resolve(__dirname, "..");
const nroPath = path.join(root, "client", "build-switch", ASSET_NAME);
const versionFile = path.join(root, "client", "VERSION");

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

// The version the .nro was compiled with (client/scripts/build_switch.sh
// defaults PIPENSX_VERSION to this file's contents), so a tag that doesn't
// match it is caught here rather than by consoles looping on an update that
// never appears to apply.
function readClientVersion() {
  const text = fs.readFileSync(versionFile, "utf8");
  const version = text.trim();
  if (!version) throw new Error(`${path.relative(root, versionFile)} is empty`);
  return version;
}

async function githubJson(url, options) {
  const res = await fetch(url, options);
  const body = await res.text();
  if (!res.ok) {
    // Deliberately reports status + body only: the request headers carry
    // the token.
    throw new Error(`${options.method} ${url} -> HTTP ${res.status}\n${body}`);
  }
  return JSON.parse(body);
}

async function uploadAsset(uploadUrlBase, headers, name, bytes) {
  const uploadUrl = `${uploadUrlBase}?name=${encodeURIComponent(name)}`;
  const uploaded = await githubJson(uploadUrl, {
    method: "POST",
    headers: {
      ...headers,
      "Content-Type": "application/octet-stream",
      "Content-Length": String(bytes.length),
    },
    body: bytes,
  });
  console.log(`Asset uploaded: ${uploaded.name} (${uploaded.size} bytes)`);
  return uploaded;
}

async function main() {
  const [tag, ...rest] = process.argv.slice(2);
  if (!tag) {
    console.error('usage: node scripts/publish-client-release.js <vX.Y.Z> ["notes" | --notes-file <path>]');
    process.exit(1);
  }

  let notes = "";
  if (rest[0] === "--notes-file") {
    if (!rest[1]) throw new Error("--notes-file needs a path");
    notes = fs.readFileSync(rest[1], "utf8");
  } else if (rest[0]) {
    notes = rest[0];
  }

  const clientVersion = readClientVersion();
  if (tag.replace(/^v/, "") !== clientVersion) {
    throw new Error(
      `tag ${tag} doesn't match the version in client/VERSION (${clientVersion}) - ` +
        `bump it and rebuild, or pass the matching tag`
    );
  }

  if (!fs.existsSync(nroPath)) {
    throw new Error(`${path.relative(root, nroPath)} not found - build the client first`);
  }
  const asset = fs.readFileSync(nroPath);
  const checksum = crypto.createHash("sha256").update(asset).digest("hex");

  const headers = {
    Authorization: `Bearer ${readToken()}`,
    Accept: "application/vnd.github+json",
    "X-GitHub-Api-Version": "2022-11-28",
    "User-Agent": "freeshop-release-script",
  };

  const release = await githubJson(`https://api.github.com/repos/${REPO}/releases`, {
    method: "POST",
    headers: { ...headers, "Content-Type": "application/json" },
    body: JSON.stringify({
      tag_name: tag,
      name: tag,
      body: notes,
      draft: false,
      prerelease: false,
    }),
  });
  console.log(`Release created: ${release.html_url}`);

  const uploadUrlBase = release.upload_url.replace(/\{.*\}$/, "");
  const uploadedNro = await uploadAsset(uploadUrlBase, headers, ASSET_NAME, asset);
  await uploadAsset(uploadUrlBase, headers, `${ASSET_NAME}.sha256`, Buffer.from(checksum, "utf8"));
  console.log(`Download URL: ${uploadedNro.browser_download_url}`);
}

main().catch((err) => {
  console.error(String(err.message || err));
  process.exit(1);
});
