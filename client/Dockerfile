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
# inside the image), so no extra package install is needed for this project.
# If you add a dependency that isn't preinstalled, add it here, e.g.:
#   RUN dkp-pacman -Syu --noconfirm && dkp-pacman -S --noconfirm <package>
FROM devkitpro/devkita64:latest

WORKDIR /workspace
