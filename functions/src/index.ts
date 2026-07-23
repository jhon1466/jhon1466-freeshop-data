import { onRequest } from "firebase-functions/v2/https";
import { createApp } from "./app";

// The whole Express app as one HTTP Function. Firebase Hosting rewrites
// /api/** here (see firebase.json); everything else (/admin, /icons,
// /downloads) is served directly by Hosting as static files.
// timeoutSeconds is raised for POST /api/admin/checksum, which streams a
// whole external download (e.g. a large .nsp on MediaFire) to hash it.
export const api = onRequest(
  { secrets: ["ADMIN_PASSWORD", "GITHUB_TOKEN"], timeoutSeconds: 540 },
  createApp()
);
