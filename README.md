# Kiln

**Kiln** (working name) is a free, open-source (MIT) GPU benchmark, feature tester, and stress/diagnostic tool for
AMD, NVIDIA and Intel GPUs on Linux and Windows.

> Status: early development. See [docs/PROGRESS.md](docs/PROGRESS.md) and [docs/PLAN.md](docs/PLAN.md).

- Scene benchmarks from legacy OpenGL 3.3 / D3D11 up to ray-traced and path-traced Vulkan / D3D12 scenes
- Feature tests: ray tracing, mesh shaders, VRS, work graphs, cooperative matrix, async compute, …
- Synthetic tests: compute throughput, memory bandwidth/latency, fill rates, geometry
- Diagnostics for the repair bench: VRAM tester, artifact detection, compute correctness, thermal soak, PCIe link check
- Detailed results: frame-time percentiles, telemetry graphs, history and comparisons, JSON/CSV/HTML export

## Install on Arch Linux (and CachyOS, EndeavourOS, Manjaro)

Run these as your normal user (not root); `makepkg` asks for your password
when it installs the package:

```sh
sudo pacman -S --needed base-devel git
git clone https://github.com/oreo1298/GPUTest.git
cd GPUTest/packaging/arch
makepkg -si
```

This builds Kiln from the checkout and installs the `kiln` app (with a **Kiln**
entry in your application menu), the `kiln-run` command-line runner and the
`kiln-hw-report` hardware report script. The build downloads its pinned
libraries and the Slang shader compiler, and takes a few minutes.

Dependencies (pulled in automatically) include `qt6-base`, `qt6-svg`,
`qt6-wayland` and `vulkan-icd-loader`. You also need the Vulkan driver for your
GPU: `vulkan-radeon` (AMD), `vulkan-intel` (Intel) or `nvidia-utils` (NVIDIA),
which also provides NVML for NVIDIA clocks, temperatures and power.

To update later, pull the new code and build again from the same folder:

```sh
cd GPUTest/packaging/arch
git pull
makepkg -sif
```

`-f` rebuilds even when the version number has not changed (it only changes for
releases). If `makepkg` says `PKGBUILD does not exist`, you are not in the
`GPUTest/packaging/arch` folder. To uninstall: `sudo pacman -R kiln`.

Each CI run also uploads the built package (`arch-package` artifact); install that one
with `sudo pacman -U kiln-*.pkg.tar.zst`.

### Other distributions

Use the AppImage (below), or build and install with CMake. You need a C++20 compiler,
CMake, Ninja, git, Qt 6 (Widgets, Sql, Svg, DBus) development packages and the
[Slang](https://github.com/shader-slang/slang/releases) shader compiler (extract a
release and point `SLANG_DIR` at it); the other libraries are fetched by CMake:

```sh
export SLANG_DIR=$HOME/slang
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr/local
cmake --build build
sudo cmake --install build
```

## Downloads
Every push to `main` builds a Linux AppImage, an Arch package and a Windows portable zip — see the latest
[CI run](https://github.com/oreo1298/GPUTest/actions/workflows/ci.yml) → Artifacts.

## Building
See [CLAUDE.md](CLAUDE.md) for the short version; full instructions will live in `docs/BUILDING.md`.

## License
MIT — see [LICENSE](LICENSE) and [THIRD_PARTY.md](THIRD_PARTY.md).
