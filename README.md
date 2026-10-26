# Kiln

**Kiln** (working name) is a free, open-source (MIT) GPU benchmark, feature tester, and stress/diagnostic tool for
AMD, NVIDIA and Intel GPUs on Linux and Windows.

> Status: early development. See [docs/PROGRESS.md](docs/PROGRESS.md) and [docs/PLAN.md](docs/PLAN.md).

- Scene benchmarks from legacy OpenGL 3.3 / D3D11 up to ray-traced and path-traced Vulkan / D3D12 scenes
- Feature tests: ray tracing, mesh shaders, VRS, work graphs, cooperative matrix, async compute, …
- Synthetic tests: compute throughput, memory bandwidth/latency, fill rates, geometry
- Diagnostics for the repair bench: VRAM tester, artifact detection, compute correctness, thermal soak, PCIe link check
- Detailed results: frame-time percentiles, telemetry graphs, history and comparisons, JSON/CSV/HTML export

## Downloads
Every push to `main` builds a Linux AppImage and a Windows portable zip — see the latest
[CI run](https://github.com/oreo1298/GPUTest/actions/workflows/ci.yml) → Artifacts.

## Building
See [CLAUDE.md](CLAUDE.md) for the short version; full instructions will live in `docs/BUILDING.md`.

## License
MIT — see [LICENSE](LICENSE) and [THIRD_PARTY.md](THIRD_PARTY.md).
