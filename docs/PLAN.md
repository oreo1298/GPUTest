# Plan

Milestones from `docs/SPEC.md` §9 as checklists. Ticked items are done and pushed.

## 1. Repo skeleton
- [x] CMake + presets (GCC, Clang, ASan, MSVC, CI variants), ccache
- [x] `scripts/cloud-setup.sh` + SessionStart hook (`.claude/settings.json`)
- [x] CI: Linux GCC/Clang (-Werror), ASan/UBSan, Windows MSVC (/WX); AppImage + portable zip artifacts
- [x] Docs: CLAUDE.md, PLAN, PROGRESS, DECISIONS, HARDWARE_TESTING, RESEARCH, THIRD_PARTY, LICENSE
- [x] clang-format / clang-tidy configs, unit-test scaffold (Catch2)

## 2. Vertical slice
- [x] Shader pipeline: Slang → SPIR-V embedded blobs (DXIL/DXBC/GLSL paths wired, enabled with their backends)
- [x] RHI interface + Vulkan backend (volk + VMA), offscreen + SDL3 window
- [x] Runner: device enumeration, `--list-devices/--list-tests`, one scene test, frame stats, GPU timestamps
- [x] Telemetry: sampler thread, NVML, amdgpu/i915/xe sysfs, VK_EXT_memory_budget
- [x] Results JSON (schema v1) + JSON-lines progress stream
- [x] Hardware report scripts (bash + PowerShell)
- [x] Lavapipe smoke test in CI with validation layers, frame dumps (PNG) + golden signatures

## 3. Qt GUI shell
- [x] Theme manager: OS light/dark + accent, live updates (portal, KDE kdeglobals, Windows 11 style)
- [x] Main window with sidebar navigation; Home (GPU list), Tests, Live run, Results, Settings pages
- [x] Runner process management (survives runner crash), basic results view
- [x] Offscreen screenshot mode for every screen (light + dark)

## 4. Full results
- [x] Custom QPainter graphs (frame times, histogram, telemetry small multiples, throttle shading), interactive (hover, zoom)
- [x] SQLite history, compare two runs / GPUs (per-test deltas, frame-time overlays, sensor deltas)
- [x] Export JSON, CSV, self-contained printable HTML (GUI + `kiln-run --export-html/--export-csv`)

## 5. Run modes
- [x] Loops: N, timed, infinite; per-loop scores; stability score + trend (live loop chart in the GUI)
- [x] Safety cutoffs (temp, hotspot, power) with clear abort messages (CI-tested with fake sensors)
- [x] Device-lost / TDR capture (VK_EXT_device_fault + last sensors), partial results, optional recover-and-continue

## 6. Diagnostics
- [x] VRAM tester (walking bits, moving inversions, random, address-in-address), failing ranges
- [x] Artifact detection under load (reference frames, mismatched pixels)
- [x] Compute correctness under load
- [x] Thermal soak + throttle-reason analysis
- [x] PCIe link check
- [x] PASS/WARN/FAIL verdicts with explanations (Diagnosis card in the GUI; fault injection proves detection in CI)

## 7. Synthetic / micro
- [x] Compute throughput FP32/FP16/FP64/INT32/INT8 (+dot products)
- [x] VRAM bandwidth (read/write/copy); cache/memory latency (pointer chase) + cache-size inference
- [x] Host↔device transfer, ReBAR/SAM detection
- [x] Pixel/ROP fill (blend, FP16), texel fill (formats, filtering, aniso)
- [x] Geometry + tessellation throughput, MSAA cost, atomics throughput

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
