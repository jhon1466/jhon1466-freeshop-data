# Build instructions

## Source checkout

This `client/` directory is a fork of [pipensx](https://github.com/i3sey/pipensx)
(see [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md)) with its `vendor/`
tree (Borealis, glm, zstd, libnx-ext, jech/dht, libutp, qrcodegen) left
uncommitted in this repo rather than pinned as submodules of it - see the
root `.gitignore`. Populate it by cloning pipensx fresh elsewhere and
copying its `vendor/` directory in:

```bash
git clone --recurse-submodules https://github.com/i3sey/pipensx.git /tmp/pipensx-src
cp -r /tmp/pipensx-src/vendor client/vendor
```

Run `make` to list the supported top-level targets.

## PC client and tests

On Debian or Ubuntu:

```bash
sudo apt-get update
sudo apt-get install -y \
  build-essential python3 libcurl4-openssl-dev libssl-dev \
  libzstd-dev zlib1g-dev
```

Build the portable command-line client or run the complete test suite:

```bash
make pc
make test
```

The client is written to `./pipensx` and accepts:

```bash
./pipensx /path/to/file.torrent [output_dir]
```

## Nintendo Switch

Requirements:

- devkitPro with devkitA64 and libnx
- CMake 3.13 or newer
- `switch-curl`, `switch-mbedtls`, `switch-zlib`, and `switch-miniupnpc`

Install them with devkitPro pacman:

```bash
sudo dkp-pacman -S switch-dev switch-curl switch-mbedtls \
  switch-zlib switch-miniupnpc
```

Build the release NRO:

```bash
export DEVKITPRO=/opt/devkitpro
make switch
```

The artifact is `build-switch/freeshop-client.nro` (an internal
`build-switch/pipensx.nro` is also produced along the way - a target name
inherited from upstream pipensx, see THIRD_PARTY_NOTICES.md - but
`freeshop-client.nro` is the one to copy to the SD card). Override CMake
when it is not on `PATH`:

```bash
make switch CMAKE_BIN=/path/to/cmake
```

Public builds contain no bundled catalog or game metadata dataset. At runtime,
catalog refresh also fetches the verified optional metadata index from the
configured pipensx-metadata manifest; failures keep the previous cache and do
not block catalog updates. A builder who needs a first-run offline
fallback may still embed a lawfully obtained compatible index explicitly:

```bash
make switch PIPENSX_METADATA_INDEX=/absolute/path/game_metadata_index.json
```

The path is build input only. Do not copy or commit the dataset under
`resources/catalog/`; that directory is ignored deliberately.

## Golden screenshot tests

Golden tests require CMake, Ninja, SDL2, ImageMagick, Xvfb, Mesa/OpenGL
development packages, and X11 development headers. On Debian or Ubuntu:

```bash
sudo apt-get install -y \
  cmake ninja-build xorg-dev libgl1-mesa-dev libglu1-mesa-dev \
  libsdl2-dev libcurl4-openssl-dev libssl-dev zlib1g-dev \
  imagemagick xvfb
make golden
```

To intentionally re-baseline screenshots, run `scripts/golden.sh update` and
review every changed PNG before committing it.

## Deploy over MTP

The deployment helper requires `gio` and an explicit target. By default it
replaces only the NRO:

```bash
make deploy MTP_DIR='mtp://DEVICE/1: SD Card/switch/freeshop-client'
```

Set `DEPLOY_CLEAN=1` to remove other remote files while preserving cached
artwork under `catalog/images/`:

```bash
make deploy MTP_DIR='mtp://DEVICE/1: SD Card/switch/freeshop-client' DEPLOY_CLEAN=1
```

## Cleanup and secret audit

```bash
make clean
make audit  # requires gitleaks in PATH
```
