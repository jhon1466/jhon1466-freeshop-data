import crypto from "node:crypto";
import express, { Router, Request, Response, NextFunction } from "express";
import { validateCatalogDocument } from "../lib/validateCatalog";
import { commitCatalog, commitFile, GithubCommitError } from "../lib/githubCommit";
import { invalidateCatalogCache } from "../lib/catalog";
import { rawUrl } from "../lib/githubConfig";

function checkAuth(req: Request): boolean {
  const expected = process.env.ADMIN_PASSWORD;
  if (!expected) return false;

  const header = req.headers.authorization || "";
  const token = header.startsWith("Bearer ") ? header.slice("Bearer ".length) : "";
  if (!token) return false;

  const a = Buffer.from(token);
  const b = Buffer.from(expected);
  return a.length === b.length && crypto.timingSafeEqual(a, b);
}

function requireAuth(req: Request, res: Response, next: NextFunction) {
  if (!checkAuth(req)) {
    res.status(401).json({ error: "unauthorized" });
    return;
  }
  next();
}

export const adminRouter = Router();

adminRouter.get("/verify", requireAuth, (_req: Request, res: Response) => {
  res.json({ ok: true });
});

adminRouter.post(
  "/catalog",
  requireAuth,
  express.json({ limit: "2mb" }),
  async (req: Request, res: Response) => {
    const result = validateCatalogDocument(req.body);
    if (!result.valid) {
      const details = (result.errors ?? [])
        .map((e) => `${e.instancePath || "/"} ${e.message}`)
        .join("; ");
      res.status(400).json({ error: "catalog_invalid", message: details });
      return;
    }

    try {
      await commitCatalog(req.body);
      invalidateCatalogCache();
      res.json({ ok: true });
    } catch (err) {
      if (err instanceof GithubCommitError) {
        res.status(502).json({ error: "github_commit_failed", message: err.message });
        return;
      }
      throw err;
    }
  }
);

const ID_PATTERN = /^[a-z0-9]+(-[a-z0-9]+)*$/;
const ICON_EXTENSIONS = ["jpg", "jpeg", "png", "webp"];
const MAX_ICON_BYTES = 2 * 1024 * 1024;

adminRouter.post(
  "/icon",
  requireAuth,
  express.json({ limit: "4mb" }),
  async (req: Request, res: Response) => {
    const { id, filename, contentBase64 } = req.body ?? {};

    if (typeof id !== "string" || !ID_PATTERN.test(id)) {
      res.status(400).json({ error: "invalid_id", message: "id must be a valid app slug" });
      return;
    }
    const ext = typeof filename === "string" ? filename.split(".").pop()?.toLowerCase() : undefined;
    if (!ext || !ICON_EXTENSIONS.includes(ext)) {
      res.status(400).json({
        error: "invalid_extension",
        message: `filename must end in one of: ${ICON_EXTENSIONS.join(", ")}`,
      });
      return;
    }
    if (typeof contentBase64 !== "string" || contentBase64.length === 0) {
      res.status(400).json({ error: "invalid_content", message: "contentBase64 is required" });
      return;
    }

    const buffer = Buffer.from(contentBase64, "base64");
    if (buffer.length === 0 || buffer.length > MAX_ICON_BYTES) {
      res.status(400).json({
        error: "invalid_size",
        message: `icon must be between 1 byte and ${MAX_ICON_BYTES} bytes`,
      });
      return;
    }

    const path = `icons/${id}.${ext}`;
    try {
      await commitFile(path, buffer, `Add/update icon for ${id}`);
      res.json({ iconUrl: rawUrl(path) });
    } catch (err) {
      if (err instanceof GithubCommitError) {
        res.status(502).json({ error: "github_commit_failed", message: err.message });
        return;
      }
      throw err;
    }
  }
);

// Only fetches fileSize, via a HEAD (falling back to a 1-byte ranged GET for
// hosts that don't answer HEAD with a Content-Length). Never downloads the
// whole file - sha256 isn't computed here anymore: for multi-GB game files,
// downloading the entire thing just to hash it was slow and prone to
// timing out (502) on this Function. sha256 is now optional in the catalog
// schema and unverified by the Switch client (see docs/catalog-schema.md) -
// paste one in by hand only if you want the extra corruption check.
adminRouter.post(
  "/filesize",
  requireAuth,
  express.json({ limit: "16kb" }),
  async (req: Request, res: Response) => {
    const { url } = req.body ?? {};
    if (typeof url !== "string" || !/^https?:\/\//i.test(url)) {
      res.status(400).json({ error: "invalid_url", message: "url must be an http(s) URL" });
      return;
    }

    // A positive integer, or null. Rejects 0/negative/NaN - a real download
    // is never 0 bytes, and some hosts answer HEAD (or a HEAD to a redirect
    // hop) with Content-Length: 0 for a file that does have a body, so a 0
    // must fall through to the ranged GET below instead of being reported.
    const parseSize = (raw: string | null | undefined): number | null => {
      if (!raw) return null;
      const n = Number(raw);
      return Number.isFinite(n) && n > 0 ? n : null;
    };

    // Some hosts (e.g. MediaFire's direct download servers) reject or
    // otherwise short-circuit requests that don't look like they come from
    // a browser - fetch()'s default has no User-Agent at all, which reads
    // as an obvious script. See the same workaround client-side in
    // client/source/net/http.c.
    const browserHeaders = {
      "User-Agent":
        "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) " +
        "Chrome/124.0.0.0 Safari/537.36",
    };

    try {
      const headRes = await fetch(url, { method: "HEAD", headers: browserHeaders });
      const headSize = headRes.ok ? parseSize(headRes.headers.get("content-length")) : null;
      if (headSize !== null) {
        res.json({ fileSize: headSize });
        return;
      }

      // Some hosts don't implement HEAD properly (or answer it with a bogus
      // 0) - ask for just the first byte instead and read the total size
      // back out of Content-Range.
      const rangeRes = await fetch(url, { headers: { ...browserHeaders, Range: "bytes=0-0" } });
      const contentRange = rangeRes.headers.get("content-range"); // "bytes 0-0/12345"
      const total = contentRange?.split("/")[1];
      const rangeSize = total && total !== "*" ? parseSize(total) : null;
      if ((rangeRes.status === 206 || rangeRes.status === 200) && rangeSize !== null) {
        res.json({ fileSize: rangeSize });
        return;
      }

      res.status(502).json({
        error: "fetch_failed",
        message: "Could not determine file size from this URL (no usable Content-Length/Content-Range)",
      });
    } catch (err) {
      res.status(502).json({
        error: "fetch_failed",
        message: `Could not reach the URL: ${err instanceof Error ? err.message : err}`,
      });
    }
  }
);
