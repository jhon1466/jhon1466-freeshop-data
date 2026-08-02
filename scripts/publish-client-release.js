// Publishes a GitHub Release on the catalog data repo with the built client
// .nro attached. The client's self-updater looks for exactly this shape -
// tag "v<CLIENT_VERSION>" and an asset named "freeshop-client.nro" (see
// client/source/config.h's CLIENT_VERSION/CLIENT_RELEASE_ASSET_NAME and
// update/self_update.h), so the tag must match the CLIENT_VERSION the .nro
// was actually built with or consoles will keep re-offering the update.
//
// Usage:
//   node scripts/publish-client-release.js v1.4.4 "release notes"
//   node scripts/publish-client-release.js v1.4.4 --notes-file notes.txt
//
// Reads GITHUB_TOKEN from server/.env (gitignored). The token is never
// printed, including in error output.
const fs = require("node:fs");
const path = require("node:path");

const REPO = "jhon1466/jhon1466-freeshop-data";
const ASSET_NAME = "freeshop-client.nro";

const root = path.resolve(__dirname, "..");
const nroPath = path.join(root, "client", ASSET_NAME);
const versionHeader = path.join(root, "client", "source", "config.h");

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

// The version the .nro was compiled with, so a tag that doesn't match it is
// caught here rather than by consoles looping on an update that never
// appears to apply.
function readClientVersion() {
  const text = fs.readFileSync(versionHeader, "utf8");
  const match = text.match(/^\s*#define\s+CLIENT_VERSION\s+"([^"]+)"/m);
  if (!match) throw new Error(`CLIENT_VERSION not found in ${path.relative(root, versionHeader)}`);
  return match[1];
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
      `tag ${tag} doesn't match CLIENT_VERSION ${clientVersion} in client/source/config.h - ` +
        `bump it and rebuild, or pass the matching tag`
    );
  }

  if (!fs.existsSync(nroPath)) {
    throw new Error(`${path.relative(root, nroPath)} not found - build the client first`);
  }
  const asset = fs.readFileSync(nroPath);

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

  const uploadUrl = `${release.upload_url.replace(/\{.*\}$/, "")}?name=${encodeURIComponent(ASSET_NAME)}`;
  const uploaded = await githubJson(uploadUrl, {
    method: "POST",
    headers: {
      ...headers,
      "Content-Type": "application/octet-stream",
      "Content-Length": String(asset.length),
    },
    body: asset,
  });
  console.log(`Asset uploaded: ${uploaded.name} (${uploaded.size} bytes)`);
  console.log(`Download URL: ${uploaded.browser_download_url}`);
}

main().catch((err) => {
  console.error(String(err.message || err));
  process.exit(1);
});
