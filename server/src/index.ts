import path from "node:path";
import express from "express";
import cors from "cors";
import { appsRouter } from "./routes/apps";
import { healthRouter } from "./routes/health";
import { loadCatalog } from "./lib/catalog";

// Fail fast on boot if the hand-edited catalog.json doesn't match the schema,
// rather than discovering it later when a Switch client hits /api/apps.
try {
  const catalog = loadCatalog();
  console.log(`Catalog OK: ${catalog.apps.length} app(s) loaded.`);
} catch (err) {
  console.error("Refusing to start: catalog.json is invalid.");
  console.error(err instanceof Error ? err.message : err);
  process.exit(1);
}

const app = express();
const PORT = Number(process.env.PORT) || 8080;

app.use(cors());

app.use("/api/apps", appsRouter);
app.use("/api/health", healthRouter);

app.use(
  "/icons",
  express.static(path.resolve(__dirname, "../public/icons"), {
    maxAge: "1d",
  })
);
app.use(
  "/downloads",
  express.static(path.resolve(__dirname, "../public/downloads"), {
    maxAge: "1d",
  })
);

app.listen(PORT, () => {
  console.log(`FreeShop server listening on http://0.0.0.0:${PORT}`);
});
