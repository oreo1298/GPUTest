# Runner command line (`kiln-run`)

The runner is the benchmark engine. Everything the GUI does goes through it, so everything is scriptable.
`kiln-run --help` lists every option; this page explains the important ones.

## Information
| Command | Output |
|---|---|
| `--list-devices [--json]` | GPUs per API (index, vendor, driver, PCI address, default choice) |
| `--list-tests [--json]` | All tests with category, supported APIs, suites, scoring |
| `--list-suites [--json]` | Preset suites and their tests |
| `--sysinfo [--api A] [--gpu N]` | Host + GPU report (JSON): CPU, RAM, OS, driver, VBIOS, PCIe link, key features |
| `--caps [--json]` | Full capability listing (extensions, features, properties, limits) |
| `--check-support [--json]` | Which tests the GPU/API can run and, if not, why |
| `--export-html IN OUT` | Convert a results file to a self-contained, printable HTML report (inline SVG charts) |
| `--export-csv IN OUT` | Convert a results file to CSV (tests, frame times per test, telemetry samples) |

## Running
```
kiln-run --suite standard --api vulkan --gpu 1 --loop 20m --out results.json
kiln-run --test scene.lattice --resolution 1440p --quality ultra --duration quick
kiln-run --suite my-suite.json --offscreen --progress jsonl
```
| Option | Meaning |
|---|---|
| `--suite, -s` | `quick`, `standard`, `extreme`, `raytracing`, `legacy`, `compute`, `diagnostics`, `full`, `smoke`, or a custom suite `.json` |
| `--test, -t` | Run specific test ids instead (comma-separated or repeated) |
| `--api, -a` | `vulkan`, `d3d12`, `d3d11`, `opengl` (default: best available) |
| `--gpu, -g` | Adapter index within the API (default: first discrete GPU) |
| `--resolution, -r` | `720p`, `1080p`, `1440p`, `4k` or `WxH` — fixed internal render resolution, independent of the window |
| `--quality, -q` | `low`, `medium`, `high`, `ultra` (suite default otherwise) |
| `--duration, -d` | `smoke`, `quick`, `standard`, `extreme` |
| `--upscale` | `off` (default), `fsr1` (AMD FSR 1: EASU + RCAS) or `bilinear`: scenes render below `--resolution` and are upscaled to it |
| `--upscale-mode` | `ultra-quality` (1.3x), `quality` (1.5x, default), `balanced` (1.7x), `performance` (2x) per axis |
| `--sharpness` | FSR 1 sharpening in stops (0 = sharpest, default 0.2) |
| `--upscaler-plugins` | Load third-party upscaler plugins from `plugins/` next to the runner (off by default); `feature.upscale` measures each (docs/PLUGINS.md) |
| `--loop, -l` | `1` (default), `N`, a duration (`20m`, `1h30m`) or `inf` |
| `--offscreen` | No window, no present |
| `--fullscreen`, `--vsync`, `--no-hud` | Window options (vsync is off by default) |
| `--window-width`, `--window-height` | Window size in pixels (default: the render resolution, shrunk to fit 80 % of the screen); the render resolution stays `--resolution` |
| `--seed N` | Workload seed; keep the default for comparable scores |
| `--validation` | Vulkan validation layers / D3D debug layer; errors are counted in the results |
| `--dump-frames DIR` | Write the golden frames of each scene as PNG |
| `--golden DIR` | Compare frames with `DIR/golden.json` (tolerant signature comparison) |
| `--param key=value` | Per-test overrides (documented per test) |

## Safety
Cutoffs are on by default: GPU 90 °C, hotspot 105 °C, memory 100 °C (sustained for 2 s). `--max-temp`,
`--max-hotspot`, `--max-mem-temp`, `--max-power` change them; `--no-safety` disables them. A cutoff stops the run
with a clear message, keeps the partial results and exits with code 4.

In loop runs, `--stability-threshold` (default 0.97) is the worst-loop ÷ best-loop score ratio below which the
run's stability check fails.

## Driver resets (device lost / TDR)
When the GPU stops responding, the run records the loss in `device_losses` (test, time, the driver's fault report
via `VK_EXT_device_fault` where available, and the last sensor sample), keeps the partial results and either stops
(`--on-device-lost abort`, default) or recreates the device and continues with the next test
(`--on-device-lost recover`). Either way the exit code is 5.

## Testing hooks (development and CI only)
- `GT_FAKE_TELEMETRY=ramp[:<°C per second>]` replaces real sensors with synthetic ones (temperature ramping from
  45 °C, power near a 300 W limit, throttle flags) to exercise the telemetry, safety and UI paths without sensors.
- `--test debug.device_lost` renders a few frames and then reports a device loss (`--param debug.frames=N`), to
  exercise device-loss handling. Tests tagged `debug` are in no suite and hidden in the GUI.
- Diagnostics fault injection (each must then report FAIL): `--param diag.vram.inject=1` (corrupts 4 words),
  `--param diag.artifacts.inject=1` (one reference frame rendered differently), `--param diag.compute.inject=1`
  (one wrong value). `--param diag.pcie.force=1` runs the PCIe check on software renderers.
- `--param feature.rt.bvh.inject=1` moves the BVH test's geometry without refitting; its probe rays must FAIL.
- `--param feature.rt.ser.baseline_only=1` / `feature.rt.omm.baseline_only=1` run the plain halves of the SER and
  opacity-micromap tests on GPUs without those features (CI on lavapipe).
- `--param feature.video.software=1` runs `feature.video` on software adapters (WARP decodes on the CPU through
  Media Foundation and can take minutes, so it is skipped by default). `--param feature.video.frames=N` decodes
  only the first N frames of the Direct3D 12 reference streams (still checked bit for bit), for decoders that
  stall part-way; `--param feature.video.dump=DIR` writes the Direct3D 12 encoded streams there
  (`d3d12_h264.h264`, ...) to examine with other decoders. `--param feature.video.inline_headers=1` (or `2`)
  also puts the H.264 / H.265 reference stream's parameter sets into the first frame's Direct3D 12 bitstream,
  ahead of the first slice entry (or inside it): DXVA passes those values in the picture parameters, and this
  shows whether a decoder wants the NAL units as well.

## Diagnostic parameters
| Parameter | Meaning |
|---|---|
| `diag.vram.mb=N` | Test N MiB instead of the preset's share of free VRAM |
| `diag.soak_s=N` | Thermal soak length in seconds (presets: smoke 3, quick 60, standard 300, extreme 1200) |

## Output
- Results JSON: `--out FILE`, default `<data dir>/results/<time>_<suite>.json` (format: [RESULTS_SCHEMA.md](RESULTS_SCHEMA.md)).
  The file is rewritten atomically after every test, so a crash or driver reset never loses finished tests.
- Progress: `--progress text` (default), `jsonl` (one JSON event per line, used by the GUI) or `quiet`.
- `--control-stdin`: the runner reads commands from stdin (`stop`); closing stdin also stops the run.
- Log: `--log-level trace|debug|info|warn|error` (default `info`), `--log-file FILE` writes it to a file as well.
- `--telemetry-interval S`: sensor sampling interval in seconds (default 0.1).

### Progress events (`--progress jsonl`)
Every line is an object with a `type`:
`hello`, `run_start`, `loop_start`, `test_start`, `progress` (fraction, status), `frames` (batch of frame times),
`telemetry` (one sample), `test_end` (full test result), `loop_end` (loop score), `log` (warnings/errors),
`safety_abort`, `device_lost`, `error`, `run_end` (status, results file, overall score).

## Exit codes
| Code | Meaning |
|---|---|
| 0 | Completed |
| 1 | Usage error |
| 2 | Setup failed (no device, driver error) or a test errored |
| 3 | Stopped by the user |
| 4 | Safety cutoff |
| 5 | Device lost (driver reset / TDR), also when the run recovered and continued |
| 6 | Validation errors with `--fail-on-validation` |
| 7 | A test reported FAIL (a diagnostic, or a feature test's correctness check) |
