# Progress log

Resume from here: read this file, then `docs/PLAN.md` for the checklist and `CLAUDE.md` for commands.

## Current state
- **Milestone 1 (skeleton): done.** CMake presets, cloud-setup + SessionStart hook, CI (Linux GCC/Clang/ASan,
  Windows MSVC) with AppImage + portable-zip artifacts, docs.
- **Milestone 2 (vertical slice): done locally, pending CI confirmation.**
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
- Next: Milestone 3 (Qt GUI shell).

## Environment notes (cloud VM)
- Blocked hosts (network policy): download.qt.io + all Qt mirrors, sdk.lunarg.com, Launchpad PPAs,
  codeload.github.com, api.anaconda.org. Allowed: github.com (git + release assets), raw.githubusercontent.com,
  archive.ubuntu.com, pypi.org, conda.anaconda.org, nuget.org.
  - Consequences: local Qt is Ubuntu's 6.4 (CI uses aqt Qt 6.8), local validation layers are 1.3.275 (the runner
    then limits itself to Vulkan 1.3 features while validating; CI uses the current LunarG layer), Mesa is 25.2.8
    from noble-updates (lavapipe = Vulkan 1.4.318: RT pipeline + ray query, mesh shaders, descriptor buffer,
    shader objects; no cooperative matrix, VRS, OMM, SER, video).
  - If download.qt.io and sdk.lunarg.com were allowed in the environment's network settings, cloud-setup.sh
    would install Qt 6.8 via aqt automatically.
- `scripts/cloud-setup.sh` takes ~3 min on a fresh VM (apt dominates). Pasting it into the cloud environment's
  setup script would make sessions start faster because the environment snapshot would already contain the
  packages; the SessionStart hook then finds everything installed and finishes in seconds.
- Use `GT_DEPS_CACHE=$HOME/.cache/gt-deps` (+ `scripts/fetch-deps.sh`) so presets share dependency clones.
- lavapipe numbers are meaningless for performance; use them for correctness only.

## Known issues / follow-ups
- Windows CI has no Vulkan driver, so the Windows smoke only checks start-up and listings until the D3D12/WARP
  backend (M10). Option: install the LunarG SDK + mesa-dist-win lavapipe on the Windows runner.
- Telemetry on Windows for AMD (ADL/D3DKMT) and Intel (Level Zero) is still to do (planned alongside M5/M10).
- Score references are provisional (docs/SCORING.md) until real hardware reports arrive.

## Log
### 2026-10-26
- Saved brief to docs/SPEC.md; research pass recorded in docs/RESEARCH.md (two research subagents).
- M1: repo skeleton, CMake presets, deps, cloud setup + hook, CI, docs, unit-test scaffold. CI green on first push.
- M2: shader pipeline, RHI + Vulkan backend, bench framework, Lattice scene, telemetry, runner CLI, results JSON,
  smoke + hardware-report scripts, golden references for lavapipe (`tests/golden/lavapipe/golden.json`).
