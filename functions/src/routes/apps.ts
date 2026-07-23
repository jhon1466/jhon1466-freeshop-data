import { Router, Request, Response } from "express";
import { loadCatalog, findApp, CatalogError } from "../lib/catalog";

export const appsRouter = Router();

appsRouter.get("/", async (_req: Request, res: Response) => {
  try {
    const catalog = await loadCatalog();
    res.json(catalog);
  } catch (err) {
    if (err instanceof CatalogError) {
      res.status(500).json({ error: "catalog_invalid", message: err.message });
      return;
    }
    throw err;
  }
});

appsRouter.get("/:id", async (req: Request, res: Response) => {
  try {
    const app = await findApp(req.params.id);
    if (!app) {
      res.status(404).json({ error: "not_found", message: `No app with id "${req.params.id}"` });
      return;
    }
    res.json(app);
  } catch (err) {
    if (err instanceof CatalogError) {
      res.status(500).json({ error: "catalog_invalid", message: err.message });
      return;
    }
    throw err;
  }
});
