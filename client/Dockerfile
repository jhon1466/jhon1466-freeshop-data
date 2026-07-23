# Builds the FreeShop Switch client without installing devkitPro natively.
#
# Usage (from the repo root):
#   docker build -t freeshop-client-builder client/
#   docker run --rm -v "${PWD}/client:/workspace" freeshop-client-builder make
#
# Produces client/freeshop-client.nro on the host.
#
# devkitpro/devkita64:latest already bundles switch-curl, switch-mbedtls and
# switch-jansson (verify with `dkp-pacman -Q | grep -E 'curl|mbedtls|jansson'`
# inside the image), so no extra package install is needed for those. It also
# bundles switch-sdl2/switch-sdl2_ttf and their transitive deps (freetype,
# harfbuzz, libpng16 - see client/Makefile's LIBS comment), so PNG icons
# already decode. switch-libjpeg-turbo is NOT bundled and is installed below
# for JPEG icon decoding (client/source/ui/ui_icons.c).
FROM devkitpro/devkita64:latest

RUN dkp-pacman -Sy --noconfirm switch-libjpeg-turbo

WORKDIR /workspace
