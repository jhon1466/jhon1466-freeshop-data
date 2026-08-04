# Third-Party Notices

FreeShop's Switch client is licensed under the GNU General Public License
v3.0 (see [`../LICENSE`](../LICENSE)). The third-party/adapted components
below retain their original licenses or, where code was adapted rather than
copied verbatim, are noted as such.

## pipensx (torrent download engine)

Source: <https://github.com/i3sey/pipensx>

The torrent download code under `client/source/torrent/` is adapted from
pipensx's `src/core/` (bencode parsing, torrent metainfo, peer wire protocol,
piece management, tracker communication) and `src/platform/storage.c`
(per-file output mapping, ported as `torrent_storage.c`). pipensx is
licensed under the GNU General Public License v3.0 - see its `LICENSE`. This
is why FreeShop's client is GPL-3.0 rather than a more permissive license:
GPL-3.0 requires any work incorporating GPL-3.0 code to itself be licensed
GPL-3.0, and a single compiled `.nro` combining this code with the rest of
the client counts as one combined work under the license, not a separable
one.

## DHT

Source: <https://github.com/jech/dht>

Juliusz Chroboczek's BitTorrent DHT (Kademlia) implementation, embedded as
`client/source/torrent/dht/` (vendored, not modified beyond what's needed to
build under devkitPro). Licensed under the MIT license - see the file
headers, which retain the original copyright notice.
