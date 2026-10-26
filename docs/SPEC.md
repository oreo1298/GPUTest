# Project brief: open-source GPU benchmark, feature tester & diagnostic suite

You are building this project mostly unattended in a Claude Code cloud session. Read the whole brief before starting.

## 0. How to work (read first)

- First, save this brief verbatim to docs/SPEC.md. Then create:
  - CLAUDE.md: build/test commands, repo layout, coding conventions (short, kept current)
  - docs/PLAN.md: the milestones below as checklists
  - docs/PROGRESS.md: running log of what's done, what's next, and known issues. Update it at the end of every milestone and after any meaningful chunk of work, so a fresh session can resume from it alone.
  - docs/DECISIONS.md: every non-trivial choice with a one-line reason
  - docs/HARDWARE_TESTING.md: what needs verification on real GPUs or Windows, with exact steps for me
- Work autonomously. Do NOT ask me about naming, architecture, libraries, UI layout, test design, or priorities. Make the call, log it in DECISIONS.md, and keep going. Don't stop between milestones to summarize or ask whether to continue.
- Only ask me when genuinely blocked: you need credentials or an account, something would cost money, or there's a licensing question the defaults below don't resolve. If you're waiting on real-hardware results, add the request to HARDWARE_TESTING.md and work on something else. Don't wait.
- Commit directly to main and push often, at minimum after every milestone and at every stable point.
  - This VM can be paused or recycled when idle, so anything not pushed can be lost.
  - New sessions start from a fresh clone of main.
  - Never force-push or rewrite history.
  - Tags can't be pushed from here; I'll push release tags myself.
- Use subagents for parallel research or independent pieces when it helps.
- Your knowledge may be out of date. Before designing the feature tests, do a short research pass on the current state of:
  - the latest Vulkan version and its ray tracing, mesh shader, cooperative matrix, shader execution reordering, and opacity micromap extensions
  - the latest D3D12 Agility SDK and Shader Model features: DXR 1.2, work graphs, sampler feedback, cooperative vectors
  - Slang's current status
  - vendor telemetry APIs: NVML, AMD ADLX, Intel IGCL / Level Zero Sysman
  - which FSR versions are open source
  Record findings in docs/RESEARCH.md.

## 1. What we're building

A free, open-source, cross-platform (Linux + Windows) GPU benchmark, feature tester, and stress/diagnostic tool for AMD, NVIDIA, and Intel GPUs. Think 3DMark, but open, more detailed, and with real diagnostics. It must exercise the newest GPU features and still run meaningfully on old GPUs.

- Name: pick a short working name yourself and define it in one place so it's easy to rename.
- License: MIT. Keep every dependency compatible with it and record all licenses in THIRD_PARTY.md.
- Do not use 3DMark's (or anyone else's) test names, scene designs, branding, or scoring formulas.

## 2. Environment, testing, and the hardware loop

- The VM:
  - This session runs on an Ubuntu 24.04 VM (~4 vCPUs, 16 GB RAM, 30 GB disk) with no GPU and no display. You have root.
- Dependencies:
  - Provision them yourself. Write scripts/cloud-setup.sh: idempotent, safe to rerun, always exits 0.
  - Run it from a SessionStart hook in .claude/settings.json that does nothing unless CLAUDE_CODE_REMOTE=true.
  - If pasting the script into the cloud environment's setup script would make new sessions start faster, say so in PROGRESS.md.
- Qt:
  - Ubuntu 24.04's Qt (6.4) is too old for the theme APIs this project needs. Install a current Qt 6 LTS with aqtinstall (pip), limited to the modules you use.
  - Never build Qt or other huge dependencies from source here. Prefer apt and prebuilt binaries.
- Builds:
  - Builds are slow on 4 cores. Use ccache, build only the targets you need, and run long commands in the background.
- GPU code in this VM:
  - Everything runs on Mesa's software drivers: lavapipe (Vulkan) and llvmpipe (OpenGL).
  - Use the newest Mesa you can install (e.g. from a PPA) so lavapipe exposes as many features as possible.
  - Use them for correctness only, at small resolutions and short durations. Performance numbers from this VM mean nothing; never tune or calibrate scores against them.
  - Anything lavapipe can't run (vendor-specific ray tracing features, matrix/ML extensions, video engines, vendor telemetry, all of D3D): write it to spec, feature-gate it so it skips cleanly when unsupported, and add it to HARDWARE_TESTING.md.
- GUI checks:
  - Run the GUI under Xvfb or Qt's offscreen platform.
  - Screenshot every screen in both light and dark themes, and look at the screenshots yourself.
- Windows:
  - You can't build Windows here. GitHub Actions windows runners are the Windows build machine; use WARP for D3D12/D3D11 smoke tests.
  - After each push, check workflow runs and job logs through the GitHub REST API with `gh api`. `gh pr` and `gh issue` won't work here because GraphQL is blocked.
  - Fix failures until CI is green.
- Test builds:
  - Every push to main must produce downloadable CI artifacts: a Linux AppImage and a Windows portable zip. I need to be able to test without building anything.
- Real-hardware loop:
  - I test on real GPUs. My main rig is CachyOS/KDE with an RTX 4090 and a Ryzen 9 9950X3D (its iGPU gives an AMD adapter too). I'll also run it on older cards and Intel GPUs from my repair bench.
  - Ship a hardware report script (bash and PowerShell) that runs a validation suite and bundles results JSON, logs, system info, and screenshots into one archive.
  - I'll commit those archives to hardware-reports/ or attach them to the session. Treat them as ground truth for bug fixes and score calibration.

## 3. Default architecture (deviate only with a logged reason)

- Toolchain: C++20, CMake with presets, Ninja.
- Two executables:
  - Runner (<name>-run): the benchmark engine and full CLI. It owns the GPU and renders in its own SDL3 window or offscreen. It emits progress as JSON lines, writes the final results file, and is fully usable headless.
  - GUI: a Qt 6 app that launches the runner as a subprocess and reads its stream. If the driver crashes or the GPU resets, the GUI survives and keeps the partial results.
- Backends:
  - Vulkan (primary, both OSes), D3D12 (Windows), plus D3D11 and OpenGL 3.3–4.6 for legacy GPUs.
  - Use a thin internal RHI for shared rendering. Feature tests may drop to native API code; never let the abstraction hide a feature.
- Shaders: one source language, compiled at build time to SPIR-V, DXIL, DXBC, and GLSL. Default to Slang; fall back to HLSL + DXC + SPIRV-Cross if Slang blocks you.
- Telemetry: load vendor libraries dynamically at runtime and degrade gracefully when one is missing. Never require a vendor SDK to build.
  - NVIDIA: NVML
  - AMD: ADLX on Windows, amdgpu sysfs/hwmon on Linux
  - Intel: IGCL / Level Zero Sysman, plus i915/xe hwmon on Linux
  - VRAM usage: DXGI and VK_EXT_memory_budget
- Results: versioned JSON schema, with local history in SQLite. No network calls, no telemetry, no accounts.

## 4. UI requirements

- Look and theme:
  - Modern, clean, and native-feeling.
  - Follow the OS light/dark mode and accent color, and update live when the system theme changes.
  - KDE: respect the Plasma color scheme. GNOME and others: follow the system preference via xdg-desktop-portal. Windows 10/11: native Windows 11 style.
- Accessibility and platforms: HiDPI and fractional scaling, Wayland and X11, fully keyboard navigable, screen-reader labels.
- Qt licensing: the open-source editions of Qt Charts, Qt Graphs, and Qt Data Visualization are GPL-only (verify). Draw graphs yourself (QPainter / Qt Quick Shapes) or use a permissively licensed library.
- Screens:
  - Home: detected GPUs (selectable, since iGPU+dGPU systems need adapter choice), driver version, feature-support badges, and one-click "run recommended suite".
  - Tests: preset suites plus a custom builder (pick tests, API, resolution, quality, duration/loop mode).
  - Live run: progress, current test, live FPS/frame-time graph, clocks, temps, power, fan, VRAM, loop counter, elapsed/remaining time, stop button. Optional HUD in the benchmark window.
  - Results: everything in section 7, with interactive graphs.
  - History & compare: past runs, plus side-by-side comparison of any two runs or GPUs.
  - System & features: full system info and a searchable, readable capability explorer. Every unsupported test explains why.
  - Settings: safety limits, units, default paths, theme override.

## 5. Tests

General rules:
- Each test declares its requirements (API, feature bits, VRAM). Unsupported tests are skipped with a reason and never crash.
- Workloads are deterministic (fixed seeds, fixed simulated time) so scores are repeatable.
- Render at a fixed internal resolution independent of window size. Options: 1080p/1440p/4K/custom, windowed or fullscreen, vsync off, and offscreen (no present).

Scene benchmarks:
- Use procedurally generated content. CC0 assets are allowed if recorded in ASSETS.md.
- Tiers:
  - Legacy (GL 3.3 / D3D11 FL 10_0+): forward/deferred rendering, shadows, tessellation where available.
  - Modern (Vulkan 1.1+ / D3D12): heavy rasterization plus async-compute post-processing.
  - Ultimate: hybrid ray tracing, a full path-traced test, mesh shaders, VRS.
- Each scene gets a CPU-light variant so it measures the GPU, not the CPU.

Feature tests (each wherever the API and hardware expose it):
- Ray tracing:
  - BVH build and refit speed
  - ray pipelines vs. inline ray queries
  - coherent vs. incoherent rays
  - shader execution reordering
  - opacity micromaps
  - any newer vendor-neutral RT features found in research
- Mesh/task (amplification) shaders vs. the traditional pipeline.
- VRS tiers and sampler feedback.
- D3D12 work graphs, plus the Vulkan equivalent if one has shipped.
- Matrix/ML: cooperative matrix, cooperative vectors, and FP16/BF16/FP8/INT8 throughput where exposed.
- Bindless/descriptor indexing, descriptor buffers, async compute overlap, multi-queue and copy queues, 64-bit atomics, subgroup/wave ops.
- Pipeline/shader compilation time and pipeline-cache effectiveness (a stutter test).
- Draw-call / API overhead, per API.
- Upscaling:
  - Open-source FSR built in.
  - DLSS and XeSS only as optional runtime-loaded plugins, off by default, and only if their licenses allow it. Verify first and log the result.
- Video engines: Vulkan Video and D3D12 Video decode/encode throughput (H.264, H.265, AV1).
- HDR output check.

Synthetic/micro tests:
- Compute throughput: FP32/FP16/FP64/INT32/INT8 (including dot products).
- VRAM bandwidth: read/write/copy across access patterns.
- Cache and memory latency via pointer chasing, inferring L1/L2/last-level cache sizes.
- Host↔device transfer bandwidth, plus Resizable BAR/SAM detection.
- Fill rates: pixel/ROP fill (with blending and FP16 targets) and texel fill (formats, filtering, anisotropic).
- Geometry: triangle/vertex throughput and tessellation throughput.
- MSAA cost and atomics throughput.

Diagnostics (important: this will be used on a repair bench to check suspect cards):
- VRAM tester: multiple patterns (walking bits, moving inversions, random, address-in-address) over a configurable share of VRAM, reporting error counts and failing address ranges.
- Artifact detection under load: render deterministic frames, check them against references, and report mismatched pixels per frame.
- Compute correctness under load: verify deterministic math results, to catch unstable overclocks or degraded silicon.
- Thermal soak with throttle-reason analysis (power, thermal, voltage, etc., wherever the vendor API reports it).
- PCIe link check: current vs. max generation and width, flagging degraded links.
- Device-lost/TDR capture: log it, save partial results, and optionally recover and continue.
- Clear PASS / WARN / FAIL verdicts with plain-language explanations.

## 6. Run modes

- Suites: presets (quick, standard, extreme, ray tracing, legacy, compute, diagnostics, full), plus custom suites saved as files.
- Loops: single run, N loops, timed loop (e.g. 20 min), or infinite until stopped.
- Stability: stress stability score = worst loop / best loop, with a configurable pass threshold, plus a trend of performance over time.
- Safety: user-configurable temperature, power, and hotspot cutoffs that abort the run with a clear message. Sensible defaults are on.
- CLI: exposes everything, e.g. <name>-run --suite standard --api vulkan --gpu 1 --loop 20m --out results.json

## 7. Results

- Scores:
  - Overall score and subscores.
  - Compare scores only within the same API and settings.
  - Document the scoring method in docs/SCORING.md.
- Per-test metrics:
  - avg/min/max FPS and 1%/0.1% lows
  - frame-time percentiles (p50/p90/p95/p99/p99.9) and std dev
  - stutter count
  - GPU busy time from timestamp queries
- Telemetry over time:
  - Graphs of core/memory clocks, temps (edge/hotspot/memory where available), power, fan, utilization, and VRAM use.
  - Averages and peaks for each.
  - Throttle events marked on the timeline.
- Efficiency (score per watt). For loop runs: per-loop scores and stability.
- System report:
  - GPU: name, vendor/device/subsystem IDs, VBIOS version where exposed, driver version, PCIe link
  - Host: CPU, RAM, OS/kernel, display server
  - Graphics: API versions and the full extension/feature lists
- Export: JSON, CSV, and a self-contained printable HTML report.

## 8. Quality gates

- CI builds cleanly with warnings-as-errors on Linux (GCC and Clang) and Windows (MSVC).
- Zero Vulkan validation-layer errors and zero D3D12 debug-layer errors in every run that's possible here or in CI.
- Unit tests (stats, scoring, schema, parsers) with Catch2 or GoogleTest, plus a sanitizer build on Linux.
- CI runs smoke tests on lavapipe/llvmpipe (Linux) and WARP (Windows).
- Golden-image checks: the runner can dump frames to PNG. Look at them yourself to confirm each test renders correctly, and keep reference hashes with tolerances.
- clang-format and clang-tidy configs committed.

## 9. Milestones

Work through these in order. Each one must end buildable, tested, pushed, and with PROGRESS.md updated.

1. Repo skeleton, CMake presets, cloud-setup script and SessionStart hook, CI for Linux + Windows with downloadable artifacts, docs files, CLAUDE.md.
2. Vertical slice: Vulkan backend, headless runner, one simple scene test, frame stats, NVML + Linux sysfs telemetry, JSON output, and the first hardware report script, so I can start testing on real GPUs early.
3. Qt GUI shell: theme following, GPU detection, run a test through the runner, basic results view.
4. Full results: graphs, SQLite history, compare, HTML/CSV export.
5. Run modes: loops, timed and infinite runs, stability score, safety cutoffs, device-lost handling.
6. Diagnostics suite.
7. Synthetic/micro tests.
8. Vulkan feature tests: RT, mesh, VRS, cooperative matrix, async compute, etc.
9. Scene benchmarks, all tiers.
10. D3D12 backend and D3D12-only features.
11. Legacy backends (OpenGL, D3D11) and the legacy suite.
12. Upscaler and video-engine tests.
13. Packaging: AppImage, Flatpak manifest, PKGBUILD, Windows installer and portable zip, a tag-triggered release workflow, README with screenshots, user docs, CONTRIBUTING.md.
14. Polish pass: UI review, performance of the tool itself, accessibility, docs.

When everything is done, write a final summary in PROGRESS.md: what works, what is untested on real hardware, and suggested next steps.
