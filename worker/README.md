# worker/

Cloudflare Worker that sits in front of the GitHub data repo
(`jhon1466/jhon1466-freeshop-data`). It forwards every request path straight
to `raw.githubusercontent.com/jhon1466/jhon1466-freeshop-data/main<path>` and
streams the response back - see [`src/index.ts`](src/index.ts). Live at
`https://freeshop-proxy.freeshopnx.workers.dev`.

The Switch client resolves relative `catalog.json`/icon/download paths
against whatever `baseUrl` a source has (see
`client/source/catalog/sources.c` / `sources.h`), so pointing that `baseUrl`
at this Worker instead of directly at `raw.githubusercontent.com` covers
most of it. The one thing that isn't relative: `/admin` writes **absolute**
`raw.githubusercontent.com` URLs into each app's `iconUrl` (see main
README's "Adding an app to the catalog"), and the client uses any URL
starting with `http` as-is rather than resolving it against `baseUrl`
(`install_common_resolve_url` in `client/source/install/install_common.c`).
So this Worker also rewrites any `raw.githubusercontent.com/jhon1466/...`
occurrences it finds inside `.json` responses to point back at itself before
returning them - see the comment in `src/index.ts`. End to end, the GitHub
username/repo never appears in `sdmc:/switch/freeshop/sources.json`, in the
compiled `.nro`'s strings, or in a packet capture of the console's traffic -
only your Worker's URL does.

It also proxies the client's self-update check the same way:
`CLIENT_RELEASES_API_URL` in `client/source/config.h` points at this
Worker's `/releases/latest` instead of straight at `api.github.com`, and
that response's asset download URL is rewritten to this Worker's own
`/releases/assets/:id` instead of GitHub's `browser_download_url`.

## Private data repo

Both proxied paths (`raw.githubusercontent.com` for catalog/icon/download
reads, `api.github.com` for the release check/asset above) work exactly the
same whether `jhon1466-freeshop-data` is public or private - the console
never holds a token either way, only this Worker does, and only once you
give it one:

```
cd worker
npx wrangler secret put GITHUB_TOKEN
```

Paste a GitHub personal access token (fine-grained, read-only, scoped to
just that repo's contents - it needs nothing else) when prompted. Wrangler
stores it as an encrypted Worker secret, not in `wrangler.toml` (which is
committed to this repo - never put a real token there). Unset it
(`npx wrangler secret delete GITHUB_TOKEN`) any time to go back to serving a
public repo with no credentials at all - every fetch in `src/index.ts` just
omits the `Authorization` header when the secret isn't set.

A plain `browser_download_url` (what GitHub's Releases API normally
returns) only works unauthenticated for a *public* repo - for a private one
it's a `github.com/.../releases/download/...` link meant for a signed-in
browser session, which a bearer token can't satisfy directly. That's why
`/releases/latest` rewrites it to `/releases/assets/:id` instead: that route
calls the API's actual asset-download endpoint
(`GET /repos/:owner/:repo/releases/assets/:id` with
`Accept: application/octet-stream`), which a token *does* work against for
a private repo.

## Deploy

Already live at `https://freeshop-proxy.freeshopnx.workers.dev` (deployed
under the `freeshopnx.workers.dev` account subdomain) and wired into
`CATALOG_BASE_URL` in
[`client/source/config.h`](../client/source/config.h) - just rebuild the
client (see main README's Quick start) to ship it. Redeploying after a code
change, or setting this up fresh on another account:

```
cd worker
npm install
npx wrangler login    # one-time, account-level - opens a browser to authorize
npm run deploy
```

`wrangler deploy` prints the live URL,
`https://freeshop-proxy.<your-workers-dev-subdomain>.workers.dev` (the
`<subdomain>` part is chosen once per Cloudflare account, under **Workers &
Pages -> your account -> Workers.dev subdomain** if you haven't set one
yet). Free plan, no custom domain or payment needed. If it changes, update
`CATALOG_BASE_URL` to match.

Note that changing `CATALOG_BASE_URL` only changes the *bootstrap default*
used to seed `sources.json` on first run - consoles that already have a
`sources.json` keep using whatever `baseUrl` is in it until that file is
edited (on-console, via the "Fuentes" screen, or by hand on the SD card).

## Local testing

```
npm run dev
```

Runs the Worker locally (`wrangler dev`) and prints a `localhost` URL you can
`curl` to confirm it mirrors `raw.githubusercontent.com/jhon1466/jhon1466-freeshop-data/main`.

## Updating

Nothing to redeploy when the catalog changes - `/admin` still commits
straight to the data repo, and this Worker reads from it live on every
request (subject to the same edge-cache TTL as `raw.githubusercontent.com`
itself, see the comment in `src/index.ts`). Only redeploy this Worker if you
change `src/index.ts` itself (e.g. to point at a different data repo).
