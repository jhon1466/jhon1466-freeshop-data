# Builds the FreeShop Switch client without installing devkitPro natively.
#
# Usage (from the repo root, after cloning Borealis into
# client/vendor/borealis/ - see client/CMakeLists.txt):
#   docker build -t freeshop-client-builder client/
#   docker run --rm -v "${PWD}/client:/workspace" freeshop-client-builder \
#     sh -c "cmake -S . -B build-cmake -DCMAKE_BUILD_TYPE=Release && cmake --build build-cmake -j4"
#
# Produces client/build-cmake/freeshop-client.nro on the host.
#
# devkitpro/devkita64:latest already bundles switch-curl, switch-mbedtls and
# switch-jansson (verify with `dkp-pacman -Q | grep -E 'curl|mbedtls|jansson'`
# inside the image), so no extra package install is needed for those.
# switch-libjpeg-turbo is NOT bundled and is installed below - the catalog
# grid's cover art (client/source/views/async_cover.cpp) decodes JPEG icons
# through it via Borealis's own Image::setImageFromMem.
FROM devkitpro/devkita64:latest

RUN dkp-pacman -Sy --noconfirm switch-libjpeg-turbo

WORKDIR /workspace
