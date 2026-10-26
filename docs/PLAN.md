# Plan

Milestones from `docs/SPEC.md` §9 as checklists. Ticked items are done and pushed.

## 1. Repo skeleton
- [x] CMake + presets (GCC, Clang, ASan, MSVC, CI variants), ccache
- [x] `scripts/cloud-setup.sh` + SessionStart hook (`.claude/settings.json`)
- [x] CI: Linux GCC/Clang (-Werror), ASan/UBSan, Windows MSVC (/WX); AppImage + portable zip artifacts
- [x] Docs: CLAUDE.md, PLAN, PROGRESS, DECISIONS, HARDWARE_TESTING, RESEARCH, THIRD_PARTY, LICENSE
- [x] clang-format / clang-tidy configs, unit-test scaffold (Catch2)

## 2. Vertical slice
- [ ] Shader pipeline: Slang → SPIR-V (+DXIL/DXBC on Windows, GLSL via SPIRV-Cross), embedded blobs
- [ ] RHI interface + Vulkan backend (volk + VMA), offscreen + SDL3 window
- [ ] Runner: device enumeration, `--list-devices/--list-tests`, one scene test, frame stats, GPU timestamps
- [ ] Telemetry: sampler thread, NVML, amdgpu/i915/xe sysfs, VK_EXT_memory_budget
- [ ] Results JSON (schema v1) + JSON-lines progress stream
- [ ] Hardware report scripts (bash + PowerShell)
- [ ] Lavapipe smoke test in CI with validation layers, frame dumps (PNG) + golden hashes

## 3. Qt GUI shell
- [ ] Theme manager: OS light/dark + accent, live updates (portal, KDE kdeglobals, Windows 11 style)
- [ ] Main window with sidebar navigation; Home (GPU list), Tests, Live run, Results, Settings pages
- [ ] Runner process management (survives runner crash), basic results view
- [ ] Offscreen screenshot mode for every screen (light + dark)

## 4. Full results
- [ ] Custom QPainter graphs (frame times, telemetry, throttle markers), interactive (hover, zoom)
- [ ] SQLite history, compare two runs / GPUs
- [ ] Export JSON, CSV, self-contained printable HTML

## 5. Run modes
- [ ] Loops: N, timed, infinite; per-loop scores; stability score + trend
- [ ] Safety cutoffs (temp, hotspot, power) with clear abort messages
- [ ] Device-lost / TDR capture, partial results, optional recover-and-continue

## 6. Diagnostics
- [ ] VRAM tester (walking bits, moving inversions, random, address-in-address), failing ranges
- [ ] Artifact detection under load (reference frames, mismatched pixels)
- [ ] Compute correctness under load
- [ ] Thermal soak + throttle-reason analysis
- [ ] PCIe link check
- [ ] PASS/WARN/FAIL verdicts with explanations

## 7. Synthetic / micro
- [ ] Compute throughput FP32/FP16/FP64/INT32/INT8 (+dot products)
- [ ] VRAM bandwidth (read/write/copy, patterns); cache/memory latency (pointer chase) + cache-size inference
- [ ] Host↔device transfer, ReBAR/SAM detection
- [ ] Pixel/ROP fill (blend, FP16), texel fill (formats, filtering, aniso)
- [ ] Geometry + tessellation throughput, MSAA cost, atomics throughput

## 8. Vulkan feature tests
- [ ] RT: BVH build/refit, pipeline vs ray query, coherent vs incoherent, SER, OMM
- [ ] Mesh/task vs traditional; VRS; work graphs (VK_AMDX_shader_enqueue)
- [ ] Cooperative matrix / vectors, FP16/BF16/FP8/INT8 throughput
- [ ] Bindless / descriptor indexing / descriptor buffer, async compute, multi-queue, copy queue, 64-bit atomics, subgroup ops
- [ ] Pipeline compile time / cache effectiveness (stutter), draw-call overhead

## 9. Scene benchmarks
- [ ] Legacy tier (forward/deferred, shadows, tessellation)
- [ ] Modern tier (heavy raster + async-compute post)
- [ ] Ultimate tier (hybrid RT, path tracing, mesh shaders, VRS)
- [ ] CPU-light variants; scoring

## 10. D3D12
- [ ] Backend, scenes, synthetic tests on D3D12
- [ ] DXR 1.1/1.2 (SER, OMM), mesh shaders, VRS, sampler feedback, work graphs, cooperative vectors
- [ ] WARP smoke tests in CI, debug layer clean

## 11. Legacy backends
- [ ] OpenGL 3.3–4.6 backend, D3D11 backend
- [ ] Legacy suite

## 12. Upscalers & video
- [ ] FSR (open-source) built in; DLSS/XeSS optional runtime plugins (license permitting)
- [ ] Vulkan Video / D3D12 Video decode/encode throughput (H.264, H.265, AV1)
- [ ] HDR output check

## 13. Packaging
- [ ] AppImage, Flatpak manifest, PKGBUILD, Windows installer + portable zip
- [ ] Tag-triggered release workflow
- [ ] README with screenshots, user docs, CONTRIBUTING.md

## 14. Polish
- [ ] UI review, tool performance, accessibility pass, docs pass
- [ ] Final summary in PROGRESS.md
