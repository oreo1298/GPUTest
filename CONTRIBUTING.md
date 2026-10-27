# Contributing

Thank you for helping. Bug reports, results from real GPUs, fixes and new tests are all welcome.

## Reporting problems and sharing results

- **Bugs**: open an issue with what you ran, what happened, and the results JSON and log (Results page → Export, or
  the file `kiln-run` printed). Driver resets and validation errors are recorded there.
- **Results from your GPU**: the hardware report (`kiln-hw-report` on Linux, `hw-report.ps1` on Windows) bundles
  everything useful into one archive; attach it to an issue. [docs/HARDWARE_TESTING.md](docs/HARDWARE_TESTING.md)
  lists the checks that still need real hardware, with what to look for.

## Changing the code

Build and test as in [docs/BUILDING.md](docs/BUILDING.md). Before sending a pull request:

- `ctest --preset <preset>` passes and `scripts/ci-smoke.sh build/<preset>` runs clean on lavapipe (CI also builds
  with Clang, AddressSanitizer and MSVC, all with warnings as errors, and runs Direct3D 12 / 11 on WARP).
- Code is formatted with `clang-format` (`.clang-format`: C++20, 4-space indent, 110 columns).
- Direct3D code compiles on Linux with `scripts/check-d3d12.sh` when you cannot build on Windows.

Conventions:

- Namespaces `gt::...`; types `PascalCase`, functions and variables `camelCase`, members `m_name`, constants
  `kName`, files `snake_case`.
- Never write the product name in code: use `gt::app::*` (generated from `cmake/AppInfo.cmake`, the only place
  the name lives), so the project can be renamed in one line.
- First-party targets call `gt_set_warnings()`.
- Every test declares its requirements and **skips with a human-readable reason** when the GPU or API cannot run it;
  it never crashes or silently passes. Feature tests check correctness, not only speed.
- Scores must stay comparable: do not change a workload without changing its reference value
  ([docs/SCORING.md](docs/SCORING.md)).
- Non-trivial decisions go in [docs/DECISIONS.md](docs/DECISIONS.md), with the reason.

### Adding a test

Tests live in `src/bench/tests/<category>/` and register themselves with `GT_REGISTER_TEST`. Look at a small one
of the same kind first (`synth.mem.bandwidth`, `feature.subgroups`, `scene.lattice`). Shaders are Slang sources in
`shaders/`, listed in `shaders/manifest.txt` and embedded at build time. Give the test an id, a description users
can understand, the APIs and suites it belongs to, a `checkSupport` that explains every skip, and, if it is scored,
a reference value.

## Licensing

Kiln is MIT-licensed; contributions are accepted under the same license. New third-party code must have a
permissive license compatible with MIT and be recorded in [THIRD_PARTY.md](THIRD_PARTY.md).
