import { Router, Request, Response } from "express";

export const dlRouter = Router();

// MediaFire's direct CDN links (download<N>.mediafire.com/...) expire on the
// order of a day, which meant re-pasting a fresh downloadUrl into the
// catalog constantly. The file's *page* URL (mediafire.com/file/<key>/<name>)
// doesn't expire - this endpoint replicates what the page's own JS does to
// turn that stable page into a fresh direct link.
//
// Two page layouts exist:
//  - Most files render the ready CDN link straight into the initial HTML as
//    <a id="downloadButton" href="...">  - just scrape it, no extra request
//    needed.
//  - Ad-gated/"deferred" downloads instead render a #deferredDownloadButton
//    with a one-time data-security-token that has to be POSTed to
//    /download_link.php, which answers {status:"success", download_url} or
//    {status:"delay", retry_after} while MediaFire's backend prepares the
//    link - retry on "delay". Kept as a fallback for pages that use it.
// Public/unauthenticated: the Switch client hits downloadUrl directly with
// no admin token, exactly like it would hit MediaFire's own CDN link.
const MEDIAFIRE_PAGE_RE = /^https:\/\/(www\.)?mediafire\.com\/file\//i;
const DOWNLOAD_BUTTON_RE = /<a\b[^>]*\bid="downloadButton"[^>]*>/i;
const HREF_RE = /\bhref="([^"]+)"/i;
const TOKEN_RE = /<button[^>]*data-security-token="([^"]+)"[^>]*>/;
const MAX_ATTEMPTS = 6;
const MAX_RETRY_AFTER_MS = 8000;

// MediaFire treats requests without a browser-looking User-Agent differently
// (see the same workaround in client/source/net/http.c and
// routes/admin.ts's /filesize).
const BROWSER_UA =
  "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) " +
  "Chrome/124.0.0.0 Safari/537.36";

function sleep(ms: number): Promise<void> {
  return new Promise((resolve) => setTimeout(resolve, ms));
}

function decodeHtmlEntities(s: string): string {
  return s.replace(/&amp;/g, "&");
}

type ResolveResult =
  | { ok: true; downloadUrl: string }
  | { ok: false; status: number; error: string; message: string };

async function resolveMediafire(pageUrl: string): Promise<ResolveResult> {
  const pageRes = await fetch(pageUrl, { headers: { "User-Agent": BROWSER_UA } });
  if (!pageRes.ok) {
    return {
      ok: false,
      status: 502,
      error: "fetch_failed",
      message: `MediaFire returned HTTP ${pageRes.status} for that page`,
    };
  }
  const html = await pageRes.text();

  const buttonMatch = html.match(DOWNLOAD_BUTTON_RE);
  const hrefMatch = buttonMatch?.[0].match(HREF_RE);
  if (hrefMatch) {
    return { ok: true, downloadUrl: decodeHtmlEntities(hrefMatch[1]) };
  }

  const tokenMatch = html.match(TOKEN_RE);
  if (!tokenMatch) {
    return {
      ok: false,
      status: 502,
      error: "parse_failed",
      message:
        "Could not find a download link or token on that MediaFire page (file removed/deleted, or MediaFire changed its page layout)",
    };
  }
  const securityToken = tokenMatch[1];
  const cookieHeader = pageRes.headers
    .getSetCookie()
    .map((c) => c.split(";")[0])
    .join("; ");

  for (let attempt = 0; attempt < MAX_ATTEMPTS; attempt++) {
    const form = new URLSearchParams({ security_token: securityToken });
    const apiRes = await fetch(new URL("/download_link.php", pageUrl), {
      method: "POST",
      headers: {
        "User-Agent": BROWSER_UA,
        "X-Requested-With": "XMLHttpRequest",
        "Content-Type": "application/x-www-form-urlencoded",
        Cookie: cookieHeader,
        Referer: pageUrl,
      },
      body: form.toString(),
    });

    const data = (await apiRes.json().catch(() => null)) as {
      result?: string;
      status?: string;
      download_url?: string;
      retry_after?: number;
      error_message?: string;
    } | null;

    if (!apiRes.ok || !data || data.result !== "success") {
      return {
        ok: false,
        status: 502,
        error: "resolve_failed",
        message: data?.error_message || `MediaFire's download API returned HTTP ${apiRes.status}`,
      };
    }

    if (data.status === "success" && data.download_url) {
      return { ok: true, downloadUrl: data.download_url };
    }

    if (data.status === "delay") {
      const waitMs = Math.min((data.retry_after || 3) * 1000, MAX_RETRY_AFTER_MS);
      await sleep(waitMs);
      continue;
    }

    return {
      ok: false,
      status: 502,
      error: "resolve_failed",
      message: `Unexpected response from MediaFire's download API (status: ${data.status})`,
    };
  }

  return {
    ok: false,
    status: 502,
    error: "resolve_timeout",
    message: "MediaFire kept the download link in a 'preparing' state past the retry limit - try again",
  };
}

dlRouter.get("/mediafire", async (req: Request, res: Response) => {
  const { url } = req.query;
  if (typeof url !== "string" || !MEDIAFIRE_PAGE_RE.test(url)) {
    res.status(400).json({
      error: "invalid_url",
      message: "url must be a https://www.mediafire.com/file/... page URL",
    });
    return;
  }

  try {
    const result = await resolveMediafire(url);
    if (!result.ok) {
      res.status(result.status).json({ error: result.error, message: result.message });
      return;
    }

    // admin's "Calcular fileSize" probe (a HEAD, or a ranged GET as a
    // fallback) chases this redirect down with the same method against the
    // resolved CDN link, which answers both directly - no need to proxy it
    // ourselves.
    res.redirect(302, result.downloadUrl);
  } catch (err) {
    res.status(502).json({
      error: "fetch_failed",
      message: `Could not reach MediaFire: ${err instanceof Error ? err.message : err}`,
    });
  }
});
