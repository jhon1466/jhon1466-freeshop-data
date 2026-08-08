// One-off helper: updates an existing GitHub Release's body (does not touch
// assets). Shares publish-client-release.js's token-reading convention.
//
// Usage:
//   node scripts/update-release-notes.js v2.0.0 --notes-file notes.txt
const fs = require("node:fs");
const path = require("node:path");

const REPO = "jhon1466/jhon1466-freeshop-data";
const root = path.resolve(__dirname, "..");

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
  return JSON.parse(body);
}

async function main() {
  const [tag, ...rest] = process.argv.slice(2);
  if (!tag) {
    console.error("usage: node scripts/update-release-notes.js <vX.Y.Z> --notes-file <path>");
    process.exit(1);
  }
  if (rest[0] !== "--notes-file" || !rest[1]) throw new Error("--notes-file <path> is required");
  const notes = fs.readFileSync(rest[1], "utf8");

  const headers = {
    Authorization: `Bearer ${readToken()}`,
    Accept: "application/vnd.github+json",
    "X-GitHub-Api-Version": "2022-11-28",
    "User-Agent": "freeshop-release-script",
  };

  const release = await githubJson(
    `https://api.github.com/repos/${REPO}/releases/tags/${tag}`,
    { method: "GET", headers }
  );

  const updated = await githubJson(
    `https://api.github.com/repos/${REPO}/releases/${release.id}`,
    { method: "PATCH", headers: { ...headers, "Content-Type": "application/json" },
      body: JSON.stringify({ body: notes }) }
  );
  console.log(`Release notes updated: ${updated.html_url}`);
}

main().catch((err) => {
  console.error(String(err.message || err));
  process.exit(1);
});
