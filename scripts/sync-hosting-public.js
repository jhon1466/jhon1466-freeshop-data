// Regenerates hosting-public/ (Firebase Hosting's static root) from admin/
// and server/public/downloads - the single sources of truth for those files.
// Icons/covers are NOT copied here: they live in the GitHub data repo
// (icons/<id>.jpg, see docs/catalog-schema.md) and are referenced by
// absolute raw.githubusercontent.com URLs in iconUrl, so they don't count
// against Firebase Hosting's storage. hosting-public/ is gitignored/
// generated; run this before `firebase deploy`.
const fs = require("node:fs");
const path = require("node:path");

const root = path.resolve(__dirname, "..");
const dest = path.join(root, "hosting-public");

fs.rmSync(dest, { recursive: true, force: true });
fs.mkdirSync(dest, { recursive: true });

fs.cpSync(path.join(root, "admin"), path.join(dest, "admin"), { recursive: true });
fs.cpSync(path.join(root, "server", "public", "downloads"), path.join(dest, "downloads"), {
  recursive: true,
});

console.log(`hosting-public/ regenerated from admin/ and server/public/downloads.`);
