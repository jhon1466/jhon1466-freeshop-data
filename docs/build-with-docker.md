# Building the Switch client (Docker / GitHub Actions)

The client is a devkitPro/libnx C/C++ project, built with CMake against
[Borealis](https://github.com/XITRIX/borealis) (the UI framework - see
`client/CMakeLists.txt`). Instead of installing devkitPro natively, this
project builds it inside devkitPro's official `devkitpro/devkita64` Docker
image, both locally and in CI.

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
git clone --depth 1 --branch moonlight_wiliwili --recurse-submodules \
  --shallow-submodules https://github.com/XITRIX/borealis.git client/vendor/borealis
docker run --rm -v "${PWD}/client:/workspace" freeshop-client-builder \
  sh -c "cmake -S . -B build-cmake -DCMAKE_BUILD_TYPE=Release && cmake --build build-cmake -j4"
```

- The first command builds an image on top of `devkitpro/devkita64`. The
  libnx packages this project needs (`switch-curl`, `switch-mbedtls`,
  `switch-jansson`) already ship in that base image — see
  `client/Dockerfile` for the note on how to add more if you need one that
  isn't preinstalled.
- The second command clones Borealis into `client/vendor/borealis/` - it's
  vendored but not committed (126MB with submodules; see the root
  `.gitignore`), so this is a one-time step per checkout, not per build.
  `client/CMakeLists.txt` fails fast with this same command if it's missing.
- The third command mounts `client/` into the container, configures CMake
  into `build-cmake/`, and builds, producing
  `client/build-cmake/freeshop-client.nro` on the host.

To rebuild from scratch:

```
docker run --rm -v "${PWD}/client:/workspace" freeshop-client-builder rm -rf build-cmake
docker run --rm -v "${PWD}/client:/workspace" freeshop-client-builder \
  sh -c "cmake -S . -B build-cmake -DCMAKE_BUILD_TYPE=Release && cmake --build build-cmake -j4"
```

For an incremental rebuild after editing source (the common case), the
`cmake --build build-cmake` half alone is enough - no need to reconfigure.

## GitHub Actions

`.github/workflows/build-client.yml` runs the equivalent build in CI on every
push/PR that touches `client/**`, using the same `devkitpro/devkita64` image
as a job container (cloning Borealis fresh each run, same as above), and
uploads the resulting `.nro` as a build artifact.

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
requests, and SD card writes - the parts that `docker build`/`cmake --build`
alone cannot verify (a clean compile only proves the code builds and links,
not that it behaves correctly at runtime).
