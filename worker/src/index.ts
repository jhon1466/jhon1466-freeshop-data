// Transparent proxy in front of the GitHub data repo. The Switch client
// resolves every catalog read (catalog.json, icons, downloads) against
// whatever `baseUrl` is in sources.json - pointing that at this Worker
// instead of straight at raw.githubusercontent.com means the repo owner/name
// never appears in sources.json, in the compiled .nro's strings, or on the
// wire. See ../README.md.
//
// Also proxies the self-update check (client/source/update/self_update.c)
// the same way: CLIENT_RELEASES_API_URL in client/source/config.h points
// here instead of straight at api.github.com, for the same reason.
const OWNER = "jhon1466";
const REPO_NAME = "jhon1466-freeshop-data";
const REPO = `${OWNER}/${REPO_NAME}`;
const UPSTREAM = `https://raw.githubusercontent.com/${REPO}/main`;
// Matches CLIENT_RELEASE_ASSET_NAME in client/source/config.h - the one
// release asset self_update.c actually looks for.
const RELEASE_ASSET_NAME = "freeshop-client.nro";

interface Env {
  // Fine-grained (or classic) PAT with read-only access to REPO. Only
  // needed once that repo is set to private - every fetch below just
  // omits the Authorization header when this isn't set, which is exactly
  // what a public repo needs anyway. Set with `wrangler secret put
  // GITHUB_TOKEN` - never put a real token in wrangler.toml, that file is
  // committed to this repo.
  GITHUB_TOKEN?: string;
}

function githubHeaders(env: Env, extra?: Record<string, string>): Headers {
  const headers = new Headers(extra);
  if (env.GITHUB_TOKEN) headers.set("Authorization", `Bearer ${env.GITHUB_TOKEN}`);
  return headers;
}

function corsHeaders(upstream: Response): Headers {
  const headers = new Headers(upstream.headers);
  headers.set("Access-Control-Allow-Origin", "*");
  return headers;
}

// GET /releases/latest - proxies api.github.com's "latest release" endpoint
// (what self_update_check() parses: tag_name, body, assets[].name/
// browser_download_url). The one rewrite: the freeshop-client.nro asset's
// browser_download_url gets pointed at this Worker's own /releases/assets/:id
// (see handleReleaseAsset) instead of GitHub's. That matters specifically
// for a private repo - GitHub's plain browser_download_url is a
// github.com/.../releases/download/... link meant for a signed-in browser
// session, not a bearer token; a token only works against the API's
// /releases/assets/:id endpoint (with Accept: application/octet-stream),
// which is what that route actually calls.
async function handleReleaseLatest(url: URL, env: Env): Promise<Response> {
  const upstream = await fetch(`https://api.github.com/repos/${REPO}/releases/latest`, {
    headers: githubHeaders(env, {
      Accept: "application/vnd.github+json",
      "User-Agent": "freeshop-proxy",
    }),
  });
  if (!upstream.ok) {
    return new Response(await upstream.text(), { status: upstream.status, headers: corsHeaders(upstream) });
  }

  const release = (await upstream.json()) as { assets?: Array<Record<string, unknown>> };
  if (Array.isArray(release.assets)) {
    for (const asset of release.assets) {
      if (asset && asset.name === RELEASE_ASSET_NAME && asset.id != null) {
        asset.browser_download_url = `${url.origin}/releases/assets/${asset.id}`;
      }
    }
  }

  return new Response(JSON.stringify(release), {
    status: 200,
    headers: { "Content-Type": "application/json", "Access-Control-Allow-Origin": "*" },
  });
}

// GET /releases/assets/:id - streams one release asset by its numeric
// GitHub id (not by filename - that's what handleReleaseLatest's rewrite
// encodes). Accept: application/octet-stream is what makes the API return
// the actual binary instead of the asset's JSON metadata; GitHub answers
// with a 302 to a signed, time-limited download URL for a private repo's
// asset, which fetch() follows on its own.
async function handleReleaseAsset(assetId: string, env: Env): Promise<Response> {
  const upstream = await fetch(`https://api.github.com/repos/${REPO}/releases/assets/${assetId}`, {
    headers: githubHeaders(env, {
      Accept: "application/octet-stream",
      "User-Agent": "freeshop-proxy",
    }),
  });
  return new Response(upstream.body, { status: upstream.status, headers: corsHeaders(upstream) });
}

export default {
  async fetch(request: Request, env: Env): Promise<Response> {
    const url = new URL(request.url);

    if (url.pathname === "/releases/latest") {
      return handleReleaseLatest(url, env);
    }
    const assetMatch = url.pathname.match(/^\/releases\/assets\/(\d+)$/);
    if (assetMatch) {
      return handleReleaseAsset(assetMatch[1], env);
    }

    const upstream = await fetch(UPSTREAM + url.pathname, {
      headers: githubHeaders(env),
      // Cloudflare edge cache - raw.githubusercontent.com already lags a
      // save by a few minutes (its own CDN), so caching a bit more here
      // doesn't meaningfully change staleness. See main README's note on
      // CATALOG_BASE_URL.
      cf: { cacheTtl: 60, cacheEverything: true },
    });

    // Strip nothing the client needs (content-type/length), just make sure
    // this is fetchable cross-origin like the raw GitHub CDN was.
    const headers = corsHeaders(upstream);

    // The admin panel writes absolute raw.githubusercontent.com URLs into
    // iconUrl (see main README's "Adding an app to the catalog"). The client
    // uses any URL starting with "http" as-is instead of resolving it
    // against the source's baseUrl (install_common_resolve_url in
    // install_common.c), so without this rewrite every icon request would
    // still go straight to GitHub even with sources.json pointed at this
    // Worker. Only buffer+rewrite JSON responses - downloads can be
    // hundreds of MB and stream through untouched.
    if (url.pathname.endsWith(".json")) {
      const text = await upstream.text();
      const rewritten = text.split(UPSTREAM).join(url.origin);
      headers.delete("content-length");
      return new Response(rewritten, { status: upstream.status, headers });
    }

    return new Response(upstream.body, { status: upstream.status, headers });
  },
};
