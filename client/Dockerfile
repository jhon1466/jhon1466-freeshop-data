# Builds the FreeShop Switch client - a fork of pipensx
# (https://github.com/i3sey/pipensx, GPL-3.0 - see THIRD_PARTY_NOTICES.md).
#
# Usage (from the repo root, after fetching vendor/ - see BUILD.md):
#   docker build -t freeshop-client-builder client/
#   docker run --rm -v "${PWD}/client:/workspace" -e DEVKITPRO=/opt/devkitpro \
#     freeshop-client-builder ./scripts/build_switch.sh
#
# Produces client/build-switch/pipensx.nro on the host.
#
# devkitpro/devkita64:latest already bundles switch-curl, switch-mbedtls,
# switch-zlib, switch-miniupnpc, switch-cmake, and deko3d/uam (verify with
# `dkp-pacman -Q` inside the image) - everything BUILD.md's "Nintendo
# Switch" section asks for except zstd, the one host-side (not
# switch-cross-compiled) tool: CMakeLists.txt shells out to it to compress
# the bundled catalog/metadata snapshots before they go into romfs.
FROM devkitpro/devkita64:latest

RUN apt-get update && apt-get install -y --no-install-recommends zstd \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /workspace
