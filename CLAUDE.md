# CLAUDE.md

Kiln (working name, defined only in `cmake/AppInfo.cmake`): open-source GPU benchmark, feature tester
and diagnostic suite. Spec: `docs/SPEC.md`. Status/next steps: `docs/PROGRESS.md` (read first when resuming).

## Build & test (Linux)
```bash
scripts/cloud-setup.sh                     # provisions deps (idempotent; runs from the SessionStart hook)
export GT_DEPS_CACHE=$HOME/.cache/gt-deps  # optional: share dependency clones between presets
scripts/fetch-deps.sh                      # optional: pre-clone pinned deps into that cache
cmake --preset gcc-debug                   # or clang-debug, gcc-release, asan, ci-linux-gcc ...
cmake --build --preset gcc-debug --target kiln-run   # build only what you need (targets: gt_runner, gt_gui, gt_unit_tests)
ctest --preset gcc-debug
scripts/ci-smoke.sh build/gcc-debug        # lavapipe smoke run (+ validation layers)
```
- Slang lives in `/opt/slang/current/bin` (found automatically). Qt: aqt Qt 6.8 in CI; locally Ubuntu Qt 6.4 is a
  compile-only fallback (download.qt.io is blocked here) — guard newer APIs with `QT_VERSION_CHECK`.
- Run long builds in the background; 4 cores. ccache is on for all Linux presets.
- GUI screenshots: `QT_QPA_PLATFORM=offscreen build/<preset>/bin/kiln --screenshots <dir>`.
- Windows builds only in CI (`.github/workflows/ci.yml`); check runs with `gh api repos/oreo1298/GPUTest/actions/runs`.

## Layout
- `src/common` (gt_common): logging, stats, scoring, results schema, sysinfo, paths. No GPU code.
- `src/telemetry`: vendor telemetry providers, all loaded at runtime (dlopen/LoadLibrary).
- `src/rhi`: thin RHI + backends (`vulkan/`, `d3d12/`, `d3d11/`, `gl/`). Tests may use native handles.
- `src/bench`: test framework, registry, suites, and all tests (`tests/<category>/`).
- `src/runner`: `kiln-run` CLI (JSON-lines progress on stdout with `--progress jsonl`).
- `src/gui`: Qt Widgets front-end; talks to the runner only via subprocess + JSON lines.
- `shaders/`: Slang sources + `manifest.txt`; compiled at build time, embedded via `gt_embed`.
- `tests/`: Catch2 unit tests. `scripts/`: setup, packaging, smoke, hardware report. `docs/`: all docs.

## Conventions
- C++20, 4-space indent, 110 cols (`.clang-format`). Namespaces `gt::...` (never the product name).
- Types PascalCase, functions/variables camelCase, members `m_name`, constants `kName`, files snake_case.
- First-party targets call `gt_set_warnings()`; CI builds with warnings as errors (GCC, Clang, MSVC).
- Never hard-code the product name in code: use `gt::app::*` (generated `gt/app_info.h`).
- Every test declares requirements and must skip (not crash) with a human-readable reason.
- Log every non-trivial decision in `docs/DECISIONS.md`; hardware-only checks go in `docs/HARDWARE_TESTING.md`.
- Commit to `main` and push after each stable step; never force-push.
