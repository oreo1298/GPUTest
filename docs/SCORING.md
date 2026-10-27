# Scoring

Scores exist to compare runs **of the same suite, API, resolution, quality and duration preset**. The results file
stores this as `scores.comparable_key` (`suite|api|WxH|quality|duration`); the GUI only compares scores whose keys
match and shows raw metrics otherwise.

## Per test
Each scored test has a primary metric (usually average FPS) and a fixed **reference value**:

- higher-is-better metrics: `score = 1000 × value / reference`
- lower-is-better metrics: `score = 1000 × reference / value`

A test that did not complete is not scored. Scenes play a fixed timeline (camera path, fixed seed) in real time,
so every GPU renders the same content at the same pace and a faster GPU renders more frames of it; the average over
the timeline is what is scored. Other frame-based tests render a fixed sequence of frames, repeated whole until the
duration preset's minimum time is reached, which keeps boost/thermal behaviour realistic without changing what is
measured.

## Aggregation
1. If a suite is looped, each test's score is averaged over the loops.
2. **Subscore** per category (scene, synthetic, feature) = geometric mean of its test scores.
3. **Overall** = geometric mean of the subscores.

Geometric means keep one outlier test from dominating and make the score scale-free (a GPU twice as fast everywhere
scores twice as high).

## Loops and stability
- **Loop score** = geometric mean of that loop's test scores.
- **Stability** = worst complete loop score / best complete loop score; it passes when ≥ the threshold
  (default 0.97, `--stability-threshold`).
- **Trend** = least-squares slope of loop scores over time, in % of the mean per hour (negative = getting slower,
  e.g. from heat soak).

## Efficiency
`per_watt` = overall score / average board power over the samples taken while scored tests ran (only when the
telemetry provider reports power).

## Frame statistics
- Average FPS = frames / total time. Min/max FPS from the slowest/fastest frame.
- 1% and 0.1% lows = 1000 / mean of the slowest 1% / 0.1% of frame times (at least one frame).
- Percentiles use linear interpolation between closest ranks.
- A stutter is a frame slower than both 2× the median of the previous 32 frames and that median + 4 ms.
- GPU busy % = mean GPU time per frame (timestamp queries) / mean frame time.

## Reference values (calibration status)
References are **provisional** until calibrated on real hardware reports (never on the CPU rasterizers used in CI).
The intent is that a mid-range 2024 GPU scores roughly 1000 per test at 1080p/high.

| Test | Metric | Reference | Status |
|---|---|---|---|
| scene.lattice | avg FPS | 600 | provisional |
| scene.courtyard | avg FPS | 200 | provisional |
| scene.courtyard.lite | avg FPS | 300 | provisional |
| scene.atrium | avg FPS | 180 | provisional |
| scene.atrium.lite | avg FPS | 250 | provisional |
| scene.skyline | avg FPS | 120 | provisional |
| scene.glasshouse | avg FPS | 90 | provisional |
| scene.gallery | avg FPS (paths per pixel and bounces scale with quality; Mpaths/s reported) | 60 | provisional |
| synth.alu.fp32 | TFLOPS | 30 | provisional |
| synth.alu.fp16 | TFLOPS | 60 | provisional |
| synth.alu.fp64 | TFLOPS | 1 | provisional |
| synth.alu.int32 | TOPS | 15 | provisional |
| synth.alu.int8dot | TOPS | 120 | provisional |
| synth.mem.bandwidth | read GB/s | 700 | provisional |
| synth.mem.atomics | G atomics/s (spread) | 150 | provisional |
| synth.transfer | upload GB/s | 25 | provisional |
| synth.fill.pixel | RGBA8 Gpixel/s | 250 | provisional |
| synth.fill.texel | RGBA8 bilinear Gtexel/s | 800 | provisional |
| synth.geometry | Gtris/s (small triangles) | 8 | provisional |
| feature.rt.bvh | BLAS build Mtri/s (fast trace) | 1000 | provisional |
| feature.rt.rays | incoherent Grays/s (best of ray query / pipeline) | 5 | provisional |
| feature.mesh | Gtri/s (best mesh-shader path) | 20 | provisional |
| feature.subgroups | G values/s (subgroup reduction) | 2000 | provisional |
| feature.bindless | Gtexel/s (per-thread table index) | 400 | provisional |
| feature.coopmat | FP16 TFLOPS (FP32 accumulate) | 150 | provisional |

Scene variants: `.lite` scenes are the CPU-light versions of the legacy scenes (instanced, few state changes);
Skyline, Glasshouse and Gallery are CPU-light by construction (GPU-driven culling, compute ray tracing), so they
have no separate variant. The heavy legacy variants stay scored because CPU-side submission cost is part of what
older games stress; the standard, extreme and legacy suites run both (quick runs only the `.lite` ones).

Unscored (reported only): `synth.mem.latency` (latency curve and cache levels), `synth.msaa` (relative cost),
all diagnostics (PASS/WARN/FAIL verdicts), and the ratio / CPU-bound feature tests: `feature.rt.ser`,
`feature.rt.omm`, `feature.vrs`, `feature.async`, `feature.pipelines`, `feature.drawcalls`, `feature.upscale`
(FSR 1 cost and PSNR), `feature.hdr` (a check), `feature.video` (fixed-function encode/decode fps). Runs with `--upscale` are scored like native runs but form their own comparable group: the
upscaler and mode are part of the comparable key.
