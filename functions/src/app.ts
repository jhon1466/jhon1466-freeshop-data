import express from "express";
import cors from "cors";
import { appsRouter } from "./routes/apps";
import { healthRouter } from "./routes/health";
import { adminRouter } from "./routes/admin";

// Mirrors server/src/app.ts, minus the /admin, /icons and /downloads static
// routes: in this deployment, Firebase Hosting serves those directly (see
// firebase.json rewrites) so this Function only ever needs to handle /api/**.
export function createApp(): express.Express {
  const app = express();

  app.use(cors());

  app.use("/api/apps", appsRouter);
  app.use("/api/health", healthRouter);
  app.use("/api/admin", adminRouter);

  return app;
}
