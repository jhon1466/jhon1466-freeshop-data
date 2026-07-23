import path from "node:path";
import express from "express";
import cors from "cors";
import { appsRouter } from "./routes/apps";
import { healthRouter } from "./routes/health";
import { adminRouter } from "./routes/admin";

// Builds the Express app without starting a listener, so it can be reused
// both by the local dev server (server/src/index.ts, app.listen) and by the
// Firebase Cloud Function entry point (functions/src/index.ts,
// onRequest(app)).
export function createApp(): express.Express {
  const app = express();

  app.use(cors());

  app.use("/api/apps", appsRouter);
  app.use("/api/health", healthRouter);
  app.use("/api/admin", adminRouter);

  // Only exercised in local dev / non-Firebase hosting - in production,
  // Firebase Hosting serves /admin and /downloads directly as static files
  // (see firebase.json), so these paths never reach the Function. Icons
  // aren't served locally either: iconUrl points at the GitHub data repo's
  // raw content directly (see docs/catalog-schema.md).
  app.use(
    "/downloads",
    express.static(path.resolve(__dirname, "../public/downloads"), {
      maxAge: "1d",
    })
  );
  app.use("/admin", express.static(path.resolve(__dirname, "../../admin")));

  return app;
}
