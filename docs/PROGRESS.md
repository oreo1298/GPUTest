# Progress log

Resume from here: read this file, then `docs/PLAN.md` for the checklist and `CLAUDE.md` for commands.

## Final summary (2026-10-27)

All fourteen milestones of the brief (docs/SPEC.md) are implemented. Every push to `main` builds and tests on
Linux (GCC, Clang, AddressSanitizer + UBSan, all with warnings as errors) and Windows (MSVC /W4 /WX), and
produces the AppImage, Flatpak bundle, Arch package, Windows installer and portable zip; a `v*` tag publishes
them as a GitHub release. About 40 000 lines of C++, 52 unit-test cases, and smoke runs on software renderers.

**What works** (verified in CI or locally, on lavapipe / llvmpipe / WARP):
- `kiln-run` (headless engine, JSON-lines progress, results JSON, HTML / CSV export) and the Qt app (Home,
  Tests, Live run, Results, History, Compare, System & features, Settings; light and dark themes following the
  desktop; accessible names and keyboard use throughout), the app talking to the runner only as a subprocess.
- Four APIs: Vulkan (everything lavapipe exposes, validation clean), Direct3D 12 (WARP with the debug layer:
  smoke, standard suite, every scene), OpenGL 3.3-4.6 (llvmpipe: standard suite, scenes match Vulkan) and
  Direct3D 11 (WARP: legacy suite, scenes match Direct3D 12).
- 43 tests plus a device-loss simulation: 8 scenes (legacy forward / deferred and their CPU-light versions,
  GPU-driven, hybrid ray-traced, path-traced), 13 synthetic tests, 17 feature tests (most check their results
  as well as their speed), 5 diagnostics; suites, scores, golden-frame checks, loops with stability and trend,
  safety cutoffs and device-loss capture / recovery (driven by fake telemetry and injected faults in CI); the
  VRAM, artifact and compute checks and the BVH test proven to FAIL on injected faults.
- Video: H.264 / H.265 / AV1 readers checked against FFmpeg's traces, parameter-set and AV1 header writers
  checked by round trips (AV1: every header of three streams rewritten bit for bit); decode and encode code for
  Vulkan Video and Direct3D 12 Video.
- Packaging: all five packages built in CI; the Arch package installed and run in an Arch container; the Flatpak
  built offline and its runner started; installer and portable zip packed with windeployqt.

**Untested on real hardware** (each item has its check in docs/HARDWARE_TESTING.md):
- Every performance number: score references are provisional until real results arrive (docs/SCORING.md).
- Telemetry and what depends on it: NVML, amdgpu / i915 / xe sensors, safety cutoffs with real sensors, the
  thermal soak, the PCIe link check, the VRAM test on real VRAM, recovery from a real driver reset.
- GPU features run only in software (WARP, lavapipe) or not at all: variable-rate shading (Vulkan per-draw,
  Direct3D 12 tier 2 images), SER, opacity micromaps, cooperative matrices, work graphs, sampler feedback, ray
  tracing and mesh shaders on real GPUs, HDR10 output on an HDR display, the XeSS plugin with Intel's library.
- All of Vulkan Video and Direct3D 12 Video (lavapipe has no video; WARP's video path hangs, see Known issues).
- Vulkan on Windows (the Windows CI runner has no Vulkan driver); AMD / Intel sensors on Windows (not
  implemented); the app on real desktops (KDE / GNOME on Wayland and X11, Windows 11, HiDPI); the installer, the
  portable zip and the Flatpak on users' machines with real GPU drivers.

**Suggested next steps**
1. Run the hardware report (`kiln-hw-report` / `hw-report.ps1`) on the RTX 4090 and the iGPU, and on any AMD or
   Intel GPU at hand; fix what it finds, then set the score references from real results.
2. Push the `v0.1.0` tag once a hardware pass is clean (the release job is ready; CHANGELOG.md has the notes).
3. Windows sensors for AMD (ADLX or D3DKMT) and Intel (IGCL / Level Zero).
4. Vulkan on the Windows CI runner (Mesa's lavapipe for Windows) so the Vulkan path is exercised there.
5. Answer the Agility SDK redistribution question (Kiln uses the system Direct3D 12 runtime until then).
6. Vulkan image-based VRS and cooperative vectors once drivers in reach support them.
7. Find out why WARP's video decode never completes, or drop the report-only CI step.
8. Optionally submit the Flatpak to Flathub (manifest ready; needs the owner's account).

## Current state
- **Milestone 1 (skeleton): done.** CMake presets, cloud-setup + SessionStart hook, CI (Linux GCC/Clang/ASan,
  Windows MSVC) with AppImage + portable-zip artifacts, docs.
- **Milestone 2 (vertical slice): done** (CI green on Linux; Windows builds since Qt install was fixed).
  - Shader pipeline: Slang → SPIR-V at build time, embedded with `gt_embed`; DXIL/DXBC/GLSL hooks in
    `cmake/Shaders.cmake` (activate with the D3D/GL backends).
  - RHI (`src/rhi/gt/rhi/rhi.h`) + Vulkan backend (volk, VMA, push descriptors or descriptor-set fallback,
    render-pass/framebuffer caches, timeline semaphores, deferred destruction, swapchain via SDL3).
  - Capability explorer data generated from vk.xml (`scripts/gen_vk_caps.py` → `src/rhi/vulkan/vk_caps_gen.inc`).
  - Bench framework: test registry, deterministic frame loop (fixed frame sequence, whole passes until the preset's
    minimum time), GPU timestamps, HUD, golden-frame dumps with tolerant signatures.
  - First scene: `scene.lattice`. Telemetry: NVML + amdgpu/i915/xe sysfs + VK memory budget, sampler thread,
    safety cutoffs. Results JSON v1, JSON-lines progress stream, CLI (`docs/CLI.md`).
  - `scripts/ci-smoke.sh` (lavapipe: offscreen + validation + golden + windowed under Xvfb),
    `scripts/hw-report.sh` / `.ps1` for real-hardware reports.
- **Milestone 3 (Qt GUI shell): done.** Theme following (Fusion + system palette, KDE kdeglobals, portal accent,
  windows11 style), sidebar pages, runner child process with JSON-lines events, live run page, results page,
  System & features explorer, settings, `kiln --screenshots DIR` for light/dark review.
- **Milestone 4 (full results): done.**
  - `gt::report` (src/common/report.*): self-contained HTML (inline SVG charts) and CSV; `kiln-run --export-html IN OUT`,
    `--export-csv IN OUT`; GUI Export menu (HTML, CSV, JSON copy, text summary to clipboard).
  - Results page tabs: Tests (table, frame-time chart + histogram, sensors during the selected test), Sensors (small
    multiples per unit with throttle shading + min/avg/peak table), Loops (score per loop, stability), System report.
  - History (`src/gui/history.*`): SQLite index over the results folder at `<data dir>/history.sqlite`, rescanned at
    start, filter, import, forget/delete, score history of comparable runs.
  - Compare page: A/B pickers, comparability warning (which settings differ), overall/sub-score/power/efficiency
    deltas, per-test deltas with ±2 % noise band, frame-time and histogram overlays, sensor deltas.
- **Milestone 5 (run modes): done.** Loops (N / timed / infinite) with per-loop scores, stability and trend; live
  "score per loop" chart; safety cutoffs with clear messages; device-loss capture (VK_EXT_device_fault report +
  last sensor sample) with abort or recover-and-continue (setting "If the GPU resets"); telemetry kept across
  recovery. Testing hooks `GT_FAKE_TELEMETRY` and `debug.device_lost` drive all of it in CI (`scripts/ci-smoke.sh`).
- **Milestone 6 (diagnostics): done.** `diagnostics` suite, run in this order: `diag.pcie` (link gen/width under
  load vs capability, host↔GPU bandwidth vs link maximum), `diag.vram` (walking ones/zeros, moving inversions,
  random, address-in-address over up to 90 % of free VRAM, failing ranges + flipped bits), `diag.compute` (integer
  chains vs CPU, float chains vs first GPU round), `diag.artifacts` (bit-exact reference frames under a ray-marched
  load), `diag.thermal` (5/20 min soak: perf drop, plateau, clocks, throttle reasons, reference checks). Analysis
  helpers in `src/common/diagnostics.*` (unit-tested). Each detector has an injection parameter used by CI.
- **Milestone 7 (synthetic): done.** 13 micro-benchmarks (`synth.alu.{fp32,fp16,fp64,int32,int8dot}`,
  `synth.mem.{bandwidth,latency,atomics}`, `synth.transfer`, `synth.fill.{pixel,texel}`, `synth.geometry`,
  `synth.msaa`), timed with `measureMicro` (calibrated repetitions per submission, median of N samples). Score
  references provisional (docs/SCORING.md). Results/Compare tables show each test's headline metric.
- **Milestone 8 (Vulkan feature tests): done.** RHI: acceleration structures + ray-tracing pipelines, mesh/task
  pipelines, per-draw VRS, texture tables (descriptor indexing), opacity micromaps (level 0), cooperative-matrix
  shape queries. Tests (each verifies its own results): `feature.rt.bvh`, `feature.rt.rays`, `feature.rt.ser`,
  `feature.rt.omm`, `feature.mesh`, `feature.vrs`, `feature.async`, `feature.subgroups`, `feature.bindless`,
  `feature.pipelines`, `feature.drawcalls`, `feature.coopmat`. On lavapipe 10 run (SER/OMM as baselines); VRS,
  SER, OMM and cooperative matrices need real hardware (docs/HARDWARE_TESTING.md). Work graphs, cooperative
  vectors and image-based VRS moved to M10 (docs/DECISIONS.md).
- **Milestone 9 (scene benchmarks): done** (CI green at 36611a8). Scene toolkit (`gt/bench/scene.h`: procedural meshes, mesh
  pool, camera paths, culling helpers) and an async-compute frame loop. Scenes: `scene.courtyard` (+`.lite`, legacy
  forward: PCF sun shadows, MSAA, tessellated terrain where available), `scene.atrium` (+`.lite`, legacy deferred
  with light volumes), `scene.skyline` (modern: GPU culling + indirect draws, cascaded shadows, async-compute AO
  and bloom), `scene.glasshouse` (ultimate hybrid: ray-queried soft shadows/AO/reflections, mesh-shader grass,
  VRS), `scene.gallery` (ultimate: path tracer with sun/portal/emitter next-event estimation and an a-trous
  denoiser; reports Mpaths/s). Skyline, Glasshouse and Gallery are CPU-light by construction (GPU-driven or
  compute-only). Provisional score references in docs/SCORING.md; CI renders every scene at 320x180 with
  validation and compares golden frames (GCC and Clang builds agree within 1, Gallery's noise within 5).
- **Arch package (requested ahead of M13): done.** `packaging/arch/PKGBUILD` builds from the checkout like the
  owner's other projects (`cd GPUTest/packaging/arch && makepkg -si`); CI job "Arch Linux package" runs makepkg in
  an `archlinux:base-devel` container, installs the package, runs a smoke suite and GUI screenshots from it, and
  uploads the `.pkg.tar.zst`. README has the install section.
- **Milestone 10 (D3D12): done.** Backend under `src/rhi/d3d12` (device/adapters, descriptor heaps, root
  signatures, resources and state tracking, graphics/compute/mesh/ray-tracing pipelines, command lists, swapchain,
  acceleration structures; Agility SDK side-by-side when present), compile-checked locally with
  `scripts/check-d3d12.sh` (mingw-w64), built with MSVC /W4 /WX in CI. D3D12-only features: VRS tier 2
  shading-rate images, `feature.sampler_feedback`, `feature.work_graphs` (HLSL via DXC), SER on DXR 1.2, OMM.
  On WARP with the debug layer the smoke suite, every scene and the whole standard suite pass (one bounded process
  per test; strict in CI). WARP shades every tile of a shading-rate image coarsely (`feature.vrs` warns and logs
  the expected and observed tile maps) — to confirm on hardware.
- **Milestone 11 (legacy backends): done.** OpenGL backend (`src/rhi/gl`, GL 3.3 core + buffer storage; compute,
  storage, multi-draw from 4.2/4.3) runs the scenes identically to Vulkan on llvmpipe and the standard suite with
  debug output as errors. Direct3D 11 backend (`src/rhi/d3d11`, FL 11_0+): record/replay like GL, CPU-side
  upload/readback memory, buffer layouts from DXBC reflection; on WARP its legacy scenes match the D3D12 frames
  (max cell difference 0–1) and the legacy suite passes with the debug layer. Legacy suite: Lattice, Courtyard,
  Atrium (+ lite), fill/texel/geometry/MSAA, fp32 ALU, memory bandwidth, draw calls.
- **Milestone 12 (upscalers & video): done.** FSR 1 (EASU + RCAS ported to Slang): `--upscale fsr1|bilinear`
  and `--upscale-mode` for scenes (GUI: Tests page), `feature.upscale` (cost per mode, PSNR against a supersampled
  reference; EASU beats bilinear on Vulkan and GL). Upscaler plugins (MIT C interface, off by default): example
  Vulkan plugin (tested in CI) and Intel XeSS (D3D12, user supplies `libxess.dll`); no DLSS (license).
  `feature.hdr` (PQ encoding check; HDR10 swapchain in a window). `feature.video` (Vulkan Video): decode fps
  on embedded 1080p reference streams (`data/video`, FFmpeg-made, frames checked by CRC against FFmpeg's
  decode) and encode fps with a GPU round-trip PSNR check; H.264, H.265 and AV1 on Vulkan; on D3D12 decode of
  all three through DXVA and encode of all three, with Kiln-written parameter sets and AV1 headers (skips
  without video engines; hardware-only, unverified).
- **Milestone 13 (packaging): done.** AppImage, Flatpak (KDE runtime, built offline in CI), Arch PKGBUILD,
  Windows installer (Inno Setup) and portable zip, all built on every push; a `v*` tag publishes them as a
  GitHub release (version checked against `APP_VERSION`, notes from CHANGELOG.md). README with screenshots,
  docs/USER_GUIDE.md, docs/BUILDING.md, CONTRIBUTING.md.
- **Milestone 14 (polish): done.** UI review from light / dark screenshots of every page, runner help and docs
  pass (every runner option documented), accessibility audit of every focusable widget, frame timing around
  golden-frame dumps, final summary above.

## Environment notes (cloud VM)
- Blocked hosts (network policy): download.qt.io + all Qt mirrors, sdk.lunarg.com, Launchpad PPAs,
  codeload.github.com, api.anaconda.org. Allowed: github.com (git + release assets), raw.githubusercontent.com,
  archive.ubuntu.com, pypi.org, conda.anaconda.org, nuget.org.
  - Consequences: local Qt is Ubuntu's 6.4 (CI: aqt Qt 6.12.0 on Linux; on Windows aqt cannot read the 6.12.0
    repository metadata, so CI falls back to the newest release it can install, currently 6.10.3), local validation layers are 1.3.275 (the runner
    then limits itself to Vulkan 1.3 features while validating; CI uses the current LunarG layer), Mesa is 25.2.8
    from noble-updates (lavapipe = Vulkan 1.4.318: RT pipeline + ray query, mesh shaders, descriptor buffer,
    shader objects; no cooperative matrix, VRS, OMM, SER, video).
  - If download.qt.io and sdk.lunarg.com were allowed in the environment's network settings, cloud-setup.sh
    would install Qt 6.12 via aqt automatically.
- `scripts/cloud-setup.sh` takes ~3 min on a fresh VM (apt dominates). Pasting it into the cloud environment's
  setup script would make sessions start faster because the environment snapshot would already contain the
  packages; the SessionStart hook then finds everything installed and finishes in seconds.
- Use `GT_DEPS_CACHE=$HOME/.cache/gt-deps` (+ `scripts/fetch-deps.sh`) so presets share dependency clones.
- lavapipe numbers are meaningless for performance; use them for correctness only.

## Known issues / follow-ups
- Windows CI has no Vulkan driver, so Vulkan on Windows is only exercised on real hardware. Option: install the
  LunarG SDK + mesa-dist-win lavapipe on the Windows runner.
- On WARP, D3D12 shading-rate images shade every tile coarsely (`feature.vrs` warn, tile maps in the log); check
  on hardware whether that is WARP or us (docs/HARDWARE_TESTING.md, M10 item).
- Vulkan shading-rate attachments (image-based VRS) and cooperative vectors (VK_NV_cooperative_vector, Slang
  CoopVec) are not implemented: no driver here or in CI supports them (docs/DECISIONS.md).
- Telemetry on Windows for AMD (ADL/D3DKMT) and Intel (Level Zero) is still to do (planned alongside M5/M10).
- Score references are provisional (docs/SCORING.md) until real hardware reports arrive.
- WARP's Direct3D 12 video path (Media Foundation on the CPU) never completes the first H.264 DecodeFrame,
  with one command list per frame and the debug layer silent about the arguments (CI now also tries it without
  the debug layer); `feature.video` skips software adapters unless `--param feature.video.software=1`, and CI
  runs it report-only. A stuck decode or encode now fails that codec after 20 s instead of hanging the run. All Direct3D 12
  video results therefore wait for real hardware (docs/HARDWARE_TESTING.md, M12 item 6).
- Windows CI uses Qt 6.10.3 instead of 6.12.0 (aqt cannot fetch 6.12.0's Windows metadata); revisit when aqt or
  the Qt repository changes — the workflow picks 6.12 automatically once it installs.

## Log
### 2026-10-27
- M10 closed: the D3D12 `feature.rt.bvh` hang on WARP was repeated BLAS builds without a barrier between them;
  standard suite now one bounded process per test, debug logs, strict; long fence waits logged; batched
  transitions of one resource chained. `feature.mesh` vertex semantic fixed.
- M11 closed: Direct3D 11 backend (record/replay, DXBC reflection for views); CI runs D3D11 smoke, legacy scenes
  against the D3D12 frames and the legacy suite on WARP. Draw-call CPU cost now includes submit (GL/D3D11 replay).
- M12: FSR 1 port, `--upscale`, GUI setting, `feature.upscale`. Upscaler plugin interface + example and XeSS
  plugins (licenses checked: XeSS headers MIT since de0fb9c, DLSS not possible). `feature.hdr`.
- M12: `feature.video` (Vulkan Video H.264 round trip), H.264 SPS/PPS/slice-header reader with unit tests,
  video codecs in `--caps`. Windows CI: the plugins no longer put `gt/bench` on the include path (its `math.h`
  shadowed the C library's under MSVC).
- M12: `feature.video` reworked: reference streams (`scripts/make-video-streams.py`), codec-independent Vulkan
  decoder/encoder (`video_vk.h`), H.264 parser checked against x264 output.
- M12: H.265 in `feature.video` (VPS/SPS/PPS/slice-header reader with RPS derivation, checked against x265
  output; Vulkan decode with RPS references; IDR + P encoder). Reference streams use fixed QP with temporal grain
  so inter frames carry residual.
- M12: AV1 in `feature.video`: OBU / sequence header / frame header reader with the reference state between
  frames (checked against FFmpeg's trace on the reference stream and two small libaom streams with tiles,
  segmentation and global motion in `tests/data/video`), Vulkan AV1 decode, key + inter-frame encoder at a
  constant q_idx; the device enables `videoEncodeAV1`.
- M12: Direct3D 12 Video decode in `feature.video` (DXVA picture parameters for H.264, H.265 and AV1 from the
  same parsers, texture-array DPB, reference-only support); shared video code split out of the Vulkan path
  (`video_common.h`, parser-side reference resolution with unit tests).
- M12: Direct3D 12 Video encode (H.264, H.265) in `feature.video`: configuration from the support queries,
  SPS / PPS / VPS written by new parameter-set writers (round-trip unit tests), metadata resolve, round trip
  through the D3D12 decoder.
- M13: Flatpak manifest (KDE 6.11, offline, CI-built bundle), Windows installer (Inno Setup), tag-triggered release
  job, CHANGELOG.md; fixed the Windows portable zip missing Qt (windeployqt never ran). README rewritten with
  screenshots; docs/USER_GUIDE.md, docs/BUILDING.md, CONTRIBUTING.md.
- M12: AV1 encode on Direct3D 12: AV1 sequence / frame header writer (every header of the three AV1 test
  streams rewritten bit for bit in the unit tests), encoder configured with the SUPPORT1 query, headers from the
  post-encode values. D3D12 video decode and encode use one command list per frame.
- M14: Compare page hides empty tiles and frame charts; empty Results page shows only the hint; readable score
  history note; top-aligned suite cards; Settings shows where system colours come from; accessible names for
  the path fields; `kiln-run --help` layout and plain `--no-hud`; docs/CLI.md lists every option; the git
  revision shown in the app no longer goes stale in incremental builds.

### 2026-10-26
- Saved brief to docs/SPEC.md; research pass recorded in docs/RESEARCH.md (two research subagents).
- M1: repo skeleton, CMake presets, deps, cloud setup + hook, CI, docs, unit-test scaffold. CI green on first push.
- M2: shader pipeline, RHI + Vulkan backend, bench framework, Lattice scene, telemetry, runner CLI, results JSON,
  smoke + hardware-report scripts, golden references for lavapipe (`tests/golden/lavapipe/golden.json`).
- M3: Qt GUI shell (theme manager, pages, runner client, charts, screenshot mode); screenshots reviewed in light/dark.
- CI: AppImage step deploys Qt Wayland plugins only if present (Qt 6.12's aqt package lacks them); Windows picks an
  installable Qt; MSVC float-narrowing warning fixed; annotations include MSVC warnings and packaging logs.
- M4: HTML/CSV export, histogram + telemetry small multiples, SQLite history, compare page, live-tinted icons,
  TabView (tab pages sized to the current page). Unit tests for histograms.
- Packaging: AppImage ships only the SQLite Qt SQL driver (qmake wrapper with an overlay plugin dir); Windows too.
- M5: device-loss records + fault reports, recovery keeps telemetry, fake sensors, debug device-loss test, CI run-mode
  checks (safety exit 4, device lost exit 5 abort/recover, stdin stop exit 3, timed loop).
- M6: five diagnostics + Diagnosis card/tile in the GUI; first compute pipelines through the RHI; explicit test run
  order (`TestInfo::order`); debug-tagged tests excluded from "full".
- Windows: D3D11 (fxc) shader fixes — `[loop]` on dynamic loops, explicit-gradient sampling in loops; per-entry
  D3D12 shader models (FP16 sm_6_2, DP4a sm_6_4, 64-bit atomics sm_6_6). No DXC is reachable from the VM, so
  D3D shader compilation is only verified by Windows CI.
- M7: synthetic tests; RHI `renderTargetSampleCounts(format)` (lavapipe has 4x but not 2x MSAA for RGBA8);
  software renderers get reduced problem sizes (lavapipe tessellation is ~1000x slower than rasterization).
- fxc: loops nested in dynamic loops and register arrays indexed in dynamic loops are spelled out with macros.
- M8: Vulkan feature tests (see Current state). DXC rejects mesh outputs passed to functions (inlined via macro);
  manifest defines are comma-separated; CI smoke runs every lavapipe-capable feature test with validation plus the
  BVH fault injection; the thermal soak no longer judges frame-rate drift on CPU rasterizers.
