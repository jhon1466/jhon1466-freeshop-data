// Transparent proxy in front of the GitHub data repo. The Switch client
// resolves every catalog read (catalog.json, icons, downloads) against
// whatever `baseUrl` is in sources.json - pointing that at this Worker
// instead of straight at raw.githubusercontent.com means the repo owner/name
// never appears in sources.json, in the compiled .nro's strings, or on the
// wire. See ../README.md.
const UPSTREAM = "https://raw.githubusercontent.com/jhon1466/jhon1466-freeshop-data/main";

export default {
  async fetch(request: Request): Promise<Response> {
    const url = new URL(request.url);
    const upstream = await fetch(UPSTREAM + url.pathname, {
      // Cloudflare edge cache - raw.githubusercontent.com already lags a
      // save by a few minutes (its own CDN), so caching a bit more here
      // doesn't meaningfully change staleness. See main README's note on
      // CATALOG_BASE_URL.
      cf: { cacheTtl: 60, cacheEverything: true },
    });

    // Strip nothing the client needs (content-type/length), just make sure
    // this is fetchable cross-origin like the raw GitHub CDN was.
    const headers = new Headers(upstream.headers);
    headers.set("Access-Control-Allow-Origin", "*");

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
