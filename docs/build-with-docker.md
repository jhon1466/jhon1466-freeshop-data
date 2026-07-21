# Building the Switch client (Docker / GitHub Actions)

The client is a devkitPro/libnx C project. Instead of installing devkitPro
natively, this project builds it inside devkitPro's official
`devkitpro/devkita64` Docker image, both locally and in CI.

## Prerequisites

- Docker Desktop installed and **running** (the engine, not just the CLI).
  Check with:
  ```
  docker info
  ```
  If that fails with a pipe/socket connection error, start Docker Desktop
  and wait for it to report "running" before continuing.

## Local build

From the repository root:

```
docker build -t freeshop-client-builder client/
docker run --rm -v "${PWD}/client:/workspace" freeshop-client-builder make
```

- The first command builds an image on top of `devkitpro/devkita64`. The
  libnx packages this project needs (`switch-curl`, `switch-mbedtls`,
  `switch-jansson`) already ship in that base image — see
  `client/Dockerfile` for the note on how to add more if you need one that
  isn't preinstalled.
- The second command mounts `client/` into the container and runs `make`,
  which produces `client/freeshop-client.nro` on the host.

To rebuild from scratch:

```
docker run --rm -v "${PWD}/client:/workspace" freeshop-client-builder make clean
docker run --rm -v "${PWD}/client:/workspace" freeshop-client-builder make
```

## GitHub Actions

`.github/workflows/build-client.yml` runs the equivalent build in CI on every
push/PR that touches `client/**`, using the same `devkitpro/devkita64` image
as a job container, and uploads the resulting `.nro` as a build artifact.

This only runs once the repository is pushed to a GitHub remote - creating
and pushing to that remote is a separate, explicit step (not done as part of
scaffolding this project).

## Testing the built .nro

Without confirmed Switch hardware, the most complete test available is
[Ryujinx](https://ryujinx.org/) (or another Switch emulator with homebrew NRO
loading support): point its virtual SD card at a local folder, run the
FreeShop backend server on the same machine/LAN, load `freeshop-client.nro`
directly, and exercise the full fetch -> list -> select -> download ->
install loop. This covers console rendering, controller input, real network
requests, and SD card writes - the parts that `docker build`/`make` alone
cannot verify (a clean compile only proves the code builds and links, not
that it behaves correctly at runtime).
