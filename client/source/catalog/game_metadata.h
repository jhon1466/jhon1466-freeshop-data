#pragma once
#include <stdbool.h>

// Fetches and parses the pipensx-metadata project's game_metadata_index.json
// (https://github.com/i3sey/pipensx-metadata) - a community-maintained,
// Nintendo eShop-sourced (English) title/description database keyed by
// BitTorrent v1 info hash, built specifically to match the switch-games
// torrent catalog this client also uses (see sources.h's
// SOURCE_KIND_TORRENT_CATALOG). The catalog's own scraped title/description
// (from RuTracker, via switch_games.json) is Russian; this is what lets
// torrent-catalog entries show an English title/description instead, the
// same way pipensx's own UI does for entries it manages to match.
//
// Loaded once per app session (memoized - the index is ~8MB, not worth
// re-fetching on every catalog refresh) via game_metadata_ensure_loaded(),
// called internally by catalog_fetch_torrent_json(). Safe to call
// unconditionally; a failed fetch/parse just means lookups return NULL and
// callers fall back to the catalog's own scraped fields, exactly like
// pipensx does for an entry the index has no match for (~19% of the
// catalog, per the project's own published match-rate stats).
bool game_metadata_ensure_loaded(void);

// info_hash_hex: uppercase hex (any case is normalized internally).
// Returns NULL if the index isn't loaded or has no match for this hash.
const char *game_metadata_find_name(const char *info_hash_hex);
const char *game_metadata_find_description(const char *info_hash_hex);
// Nintendo eShop CDN cover art (img-eshop.cdn.nintendo.net) - preferred
// over the catalog's own scraped cover (imageban.ru/fastpic.org, both
// noticeably slower and less reliable to fetch from than Nintendo's CDN).
const char *game_metadata_find_icon_url(const char *info_hash_hex);
