# Building from source

Kiln is C++20 with CMake. The third-party libraries (nlohmann/json, CLI11, Catch2, Vulkan-Headers, volk, VMA, SDL3,
SPIRV-Cross, DirectX-Headers) are fetched at configure time at pinned versions ([THIRD_PARTY.md](../THIRD_PARTY.md)),
so you install only the toolchain, Qt and the Slang shader compiler.

## Linux

Needs: GCC 13+ or Clang 17+, CMake 3.25+, Ninja, git, Qt 6 development packages (Widgets, Sql, Svg, DBus; Qt 6.8 or
later is the target, 6.4 compiles), and [Slang](https://github.com/shader-slang/slang/releases) — extract a release
and point `SLANG_DIR` at it (or put `slangc` on `PATH`). The version CI uses is `SLANG_VERSION` in
`.github/workflows/ci.yml`.

Ubuntu 24.04:

```sh
sudo apt install build-essential cmake ninja-build git pkg-config \
    qt6-base-dev qt6-svg-dev libqt6sql6-sqlite qt6-wayland \
    libgl-dev libegl-dev libx11-dev libxext-dev libxrandr-dev libxcursor-dev libxfixes-dev libxi-dev \
    libxss-dev libxtst-dev libxkbcommon-dev libwayland-dev wayland-protocols libdecor-0-dev libdrm-dev \
    libgbm-dev libdbus-1-dev libudev-dev mesa-vulkan-drivers
export SLANG_DIR=$HOME/slang
cmake --preset gcc-release
cmake --build --preset gcc-release
ctest --preset gcc-release
build/gcc-release/bin/kiln
```

Presets: `gcc-debug`, `gcc-release`, `clang-debug`, `clang-release`, `asan` (AddressSanitizer + UBSan). Build one
target with `--target kiln-run` (runner), `gt_gui` (app) or `gt_unit_tests`. To install system-wide:

```sh
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr/local
cmake --build build && sudo cmake --install build
```

To share the dependency clones between build folders, set `GT_DEPS_CACHE` to a directory and run
`scripts/fetch-deps.sh` once.

## Windows

Needs: Visual Studio 2022 (Desktop C++), CMake and Ninja (both come with Visual Studio), Qt 6 for MSVC 64-bit (Qt
Online Installer or `aqt`), Slang for Windows (`SLANG_DIR`) and, for the work-graph and newest shader-model tests, a
DXC from the `Microsoft.Direct3D.DXC` NuGet package (`DXC_DIR`; older DXCs skip those shaders). From a *x64 Native
Tools* prompt:

```bat
set SLANG_DIR=C:\slang
set CMAKE_PREFIX_PATH=C:\Qt\6.12.0\msvc2022_64
cmake --preset msvc-release
cmake --build --preset msvc-release
```

The Direct3D 12 Agility SDK is not shipped (see docs/DECISIONS.md); Kiln uses the system Direct3D 12 runtime.

## Packages

| Package | Command |
|---|---|
| AppImage | `scripts/package-appimage.sh build/gcc-release dist` |
| Windows portable zip + installer | `pwsh scripts/package-windows.ps1 -BuildDir build/msvc-release -OutDir dist -Installer` (needs `windeployqt` on `PATH`; installs Inno Setup with Chocolatey if missing) |
| Flatpak | `flatpak-builder --user --install --install-deps-from=flathub --force-clean build-flatpak packaging/flatpak/io.github.oreo1298.Kiln.yml` |
| Arch package | `cd packaging/arch && makepkg -si` |

CI builds all of them on every push to `main`; pushing a `v<version>` tag also publishes them as a GitHub release
(the version must match `APP_VERSION` in `cmake/AppInfo.cmake`, and `CHANGELOG.md` gives the notes).

## Checks

- Unit tests: `ctest --preset <preset>`.
- Smoke run on a software renderer (lavapipe, with validation layers): `scripts/ci-smoke.sh build/<preset>`.
- Direct3D 12 code on Linux: `scripts/check-d3d12.sh` compile-checks it with mingw-w64.
- GUI screenshots: `QT_QPA_PLATFORM=offscreen build/<preset>/bin/kiln --screenshots <dir>`.
