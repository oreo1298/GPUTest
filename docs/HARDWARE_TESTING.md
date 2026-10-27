# Hardware testing

Things that cannot be verified in the cloud VM (no GPU, no Windows desktop) and need a real machine.
Each entry: what, why, exact steps, and what to send back. The quickest way to send everything back is the
hardware report script (milestone 2+): it runs the validation suite and bundles results, logs, system info
and screenshots into one archive — commit it under `hardware-reports/` or attach it to the session.

## How to get a build
- Linux: download the `linux-appimage` artifact from the latest successful CI run on `main`
  (Actions → CI → run → Artifacts), unzip, `chmod +x Kiln-*.AppImage`.
  - GUI: `./Kiln-*.AppImage`; runner CLI: `./Kiln-*.AppImage run --help`.
- Windows: download `windows-portable`, unzip anywhere, run `kiln.exe` (GUI) or `kiln-run.exe` (CLI).
- Arch / CachyOS: `cd GPUTest/packaging/arch && makepkg -si` from a clone (see README), or `sudo pacman -U` the
  `arch-package` artifact. Then `kiln` (GUI, also in the app menu), `kiln-run` (CLI), `kiln-hw-report`.

## The hardware report (start here)
- Linux: `kiln-hw-report` when installed from the Arch package, `./hw-report.sh` from the repo `scripts/`
  directory, or from inside the AppImage:
  `./Kiln-*.AppImage --appimage-extract >/dev/null && squashfs-root/usr/share/kiln/hw-report.sh --runner squashfs-root/usr/bin/kiln-run`
- Windows (portable zip folder): `powershell -ExecutionPolicy Bypass -File hw-report.ps1`
- Options: `--standard` / `-Mode standard` (longer), `--stress 20m` / `-Mode stress -Stress 20m`, `--api`, `--gpu`.
- It runs, for every GPU and API: `--sysinfo`, `--caps`, `--check-support`, the smoke suite with validation layers
  and frame dumps, then the quick suite with frame dumps. Output: `hwreport-<host>-<time>.tar.gz` / `.zip`.

## Pending checks

### Arch package (requested)
On the CachyOS rig: install from a clone (`cd GPUTest/packaging/arch && makepkg -si`). Check that **Kiln** appears
in the KDE application menu with its icon, that it starts on Wayland, that `kiln-run --list-devices` lists the 4090
and the iGPU, and that `kiln-hw-report` finds the runner by itself. Tell me if makepkg complained about anything
(missing dependency, namcap warnings) — CI builds it in a clean Arch container, but CachyOS's compiler flags differ.

### M12 — upscaling
1. **FSR 1 in scenes**: `kiln-run --test scene.courtyard --upscale fsr1 --upscale-mode quality` windowed at
   1440p or 4K, and the same with `--upscale bilinear`: FSR should look sharper, with no shifted or ringing
   edges. Compare the fps with a native run (`--upscale off`).
2. **feature.upscale** on each API (`--api vulkan`, `d3d12`, `d3d11`, `opengl`): verdict pass, FSR 1 cost per mode
   (`fsr1_*_ms`) at 4K output, and EASU PSNR at least bilinear's.
3. **XeSS plugin (Windows, any GPU with DP4a)**: copy `libxess.dll` from https://github.com/intel/xess (`bin`) into
   the zip's `plugins` folder and run `kiln-run.exe --test feature.upscale --upscaler-plugins --api d3d12`.
   `details.plugins` should show XeSS "measured" with `psnr_last_frame` above `psnr_first_frame` (it accumulates
   detail over the jittered sequence); if it is lower, the jitter or motion-vector sign convention needs flipping.

4. **HDR output** (`feature.hdr`, windowed): with HDR on (Windows: Settings > Display > Use HDR; KDE Plasma 6
   Wayland: Display Configuration > High Dynamic Range), run `kiln-run --test feature.hdr --duration quick` on
   Vulkan and D3D12. For 5 s the window shows 16 grey bars (0 to 10000 nits) and six colour patches. Tell me:
   the verdict and `display_*` details, up to which bar the bars still get brighter (about the display's peak),
   and whether the first three colour patches look more saturated than the last three (wide gamut). With HDR
   off the verdict should be a warning, and the run should continue normally.

### M11 — OpenGL and Direct3D 11
1. **OpenGL on the 4090 and the iGPU (Linux)**: `kiln-run --api opengl --suite legacy --duration quick` and the
   same windowed (drop `--offscreen`). The scenes should look like their Vulkan frames; the HUD should read
   "[OpenGL]". Send `results/*.json` and the log; note any `[gl-debug]` errors.
2. **Direct3D 11 (Windows, any GPU)**: `kiln-run.exe --api d3d11 --suite legacy --duration quick --validation`
   (the debug layer needs Windows' optional feature "Graphics Tools"; without it the log says so) and a windowed
   run of `--test scene.courtyard`. Check that frames match the D3D12 run's, that the HUD shows, that resizing
   the window works, and that `--caps --api d3d11` lists the feature level. Send the results JSON and the logs.

### M10 — Direct3D 12 (Windows)
On a Windows PC (the 4090 if it dual-boots, otherwise any GPU), unzip the `windows-portable` artifact and run
`powershell -ExecutionPolicy Bypass -File hw-report.ps1 -Mode standard` (it covers Vulkan and Direct3D 12).
1. `kiln-run --list-devices`: every GPU should appear under Direct3D 12 too, with "FL 12_2, SM 6.x" (6.8+ on
   current drivers) and the right driver version (NVIDIA shown as e.g. "566.36").
2. **Same GPU, both APIs**: compare the Vulkan and Direct3D 12 results of the scenes; frame rates within ~15 % of
   each other are plausible. Frames (`frames\d3d12-gpu0-*`) must look like the Vulkan ones.
3. **Window**: `kiln-run --api d3d12 --test scene.skyline` (no `--offscreen`): the log's present mode should be
   "immediate (tearing)" with VSync off. Resize/minimise the window during the run — no crash.
4. **Debug layer** (optional): install "Graphics Tools" (Settings → System → Optional features), then run
   `kiln-run --api d3d12 --suite smoke --validation`; the results file's `validation.errors` must be 0.
5. **Runtime**: the device log line ends with "Windows built-in runtime"; tell me your Windows build (winver).
   Shader model 6.9 features (SER, opacity micromaps) need the Agility SDK, which is not shipped yet (see below).
6. **VRS tier 2**: `kiln-run --api d3d12 --test feature.vrs --dump-frames vrs`: details should show
   `shading_rate_image` "yes (16x16 px tiles)" (8 on some GPUs), `speedup_image` above 1, and a Pass verdict
   ("Rate image: ~100 % of blocks in coarse tiles and ~0 % in 1x1 tiles shaded once").
7. **Sampler feedback**: `kiln-run --api d3d12 --test feature.sampler_feedback`: Pass ("All 256 feedback regions
   decode to the mip level that was sampled"); a Warn about conservative feedback is acceptable — tell me which.
   Note `feedback_overhead_pct` (writing feedback on top of the same samples).
8. **Work graphs** (RTX 4090 with a 2024+ driver; Radeon 7000-series iGPU/dGPU with Adrenalin 24.x+):
   `kiln-run --api d3d12 --test feature.work_graphs`: Pass ("All 1048576 records ... ran exactly once"); send
   `graph_mrecords_s` and `graph_vs_dispatch`. A skip should name the missing tier or shader model.
9. **DXR 1.2** (needs the Agility SDK runtime, see below; driver 572+ on the 4090):
   `kiln-run --api d3d12 --test feature.rt.ser,feature.rt.omm`: both should run (not skip) with Pass verdicts;
   send `ser_speedup`, `omm_speedup` and `omm_tests_per_ray` for comparison with the Vulkan run.

**Decision needed (licensing):** shipping Microsoft's Agility SDK runtime (`D3D12Core.dll`,
`d3d12SDKLayers.dll`) is allowed, but its license makes the distributor require end-user terms that protect
Microsoft and indemnify Microsoft against claims. Kiln works without it (Windows' own runtime). If you are fine
with those terms, say so and the portable zip will include it (newest shader models, debug layer without
Graphics Tools).

### M9 — scene benchmarks
On the RTX 4090 (and once on the Radeon iGPU): `kiln-run --suite standard --dump-frames frames-std` and
`kiln-run --suite raytracing --dump-frames frames-rt`; on the 4090 also `--suite extreme`. Send the results files and
the frames folders (or just run `kiln-hw-report --standard`).
1. **Frames**: every scene's PNGs should look like CI's (`smoke-linux-gcc` artifact → `scene-frames/`): Courtyard
   (cloister in a meadow, lamp posts), Atrium (glass-roofed hall with balconies), Skyline (city at dusk with lit
   windows), Glasshouse (steel conservatory, grass beds, chrome sculpture), Gallery (sculpture hall, red wall).
   Black, garbled or flickering frames are bugs.
2. **Glasshouse details**: `grass_path` should be "mesh shaders" and `variable_rate_shading` true on the 4090
   (and mesh shaders on RADV). Do the grass blades look the same as in CI's vertex/mesh frames?
3. **Gallery**: at high (4 paths, 4 bounces) and ultra (8 paths) the image should be much smoother than CI's
   1-path frames; note the Mpaths/s metric and whether any fireflies (isolated bright dots) remain.
4. **Calibration**: all scene references are guesses (docs/SCORING.md); your FPS at 1080p/high sets them.
5. **CPU-heavy vs lite**: Courtyard/Atrium heavy variants measure draw submission; note the gap to `.lite`.
6. **Windowed**: watch Skyline (async compute) and Glasshouse in the window (no `--offscreen`): any stutter,
   flicker or tearing artefacts?

### M8 — feature tests
Run `kiln-run --suite raytracing` and `kiln-run --test feature.mesh,feature.vrs,feature.async,feature.subgroups,feature.bindless,feature.pipelines,feature.drawcalls,feature.coopmat`
on the RTX 4090 and on the Radeon iGPU, with `--validation` once and without it once, and send both results files.
These features are not in the CI rasterizer, so this is their first real run:
1. **Every feature test should PASS** — each one checks its own results (probe rays, identical hits/images,
   exact matrix products). A FAIL is either a driver bug or mine; the details say which values differed.
2. **SER** (`feature.rt.ser`, 4090): speed-up > 1 and "not a single shaded value" changed. If it is skipped with
   "only NVIDIA's own SER extension", tell me the driver version.
3. **Opacity micromaps** (`feature.rt.omm`, 4090): identical hits; alpha tests per ray should drop a lot.
4. **Cooperative matrices** (4090): FP16 ≈ 165, FP8 ≈ 330 TFLOPS dense, INT8 ≈ 660 TOPS; the details list every
   shape the driver offers — send that list (it decides which shapes I compile next).
5. **VRS**: speed-up at 2x2 ≈ 2–3.5x and "Coarse shading verified". A WARN means the driver ignored the rate.
6. **Mesh shaders**: images match; note the task-shader culled share and the speed-ups vs the vertex pipeline.
7. **Async / copy overlap**: gains between 1.0 and ~1.8 are plausible; below 1.0 is interesting — tell me.
8. **Pipelines / draw calls**: compare with your feel of shader-compile stutter in games; draw-call numbers are
   CPU-bound (your 9950X3D) and mostly useful to compare APIs later.

### M7 — synthetic tests
Run `kiln-run --suite compute` and `kiln-run --test synth.fill.pixel,synth.fill.texel,synth.geometry,synth.msaa` on
the RTX 4090 (and the iGPU if you like) and send the results. Sanity ranges for a 4090: FP32 ≈ 60–82 TFLOPS,
FP16 about the same (no 2x on Ada for non-tensor math), FP64 ≈ 1.3 TFLOPS, memory read ≈ 850–1000 GB/s, latency
curve with steps near 128 KiB (L1) and 72 MiB (L2), upload ≈ 22–26 GB/s on Gen4 x16, Resizable BAR on/off as set
in the BIOS. Numbers far outside these ranges point to a measurement problem — tell me which.

### M6 — diagnostics
Run `kiln-run --suite diagnostics` (Standard: ~8 minutes) on the RTX 4090 and on the iGPU, or in the GUI pick the
Diagnostics suite. Send the results files. Check:
1. **VRAM test** covers ~90 % of free VRAM on the 4090 (details → `tested_mb`, `free_mb`) and passes; how long did it
   take, and did the desktop stay responsive?
2. **PCIe**: the verdict should say "PCIe Gen4 x16 (card supports Gen4 x16)" on the 4090 with ~20–26 GB/s host → GPU.
   If you have a riser or an x8 slot, try it: the check should WARN.
3. **Thermal soak**: plausible plateau temperature/time, power-limit share, and no thermal throttling on a healthy
   card. Compare with what your usual monitoring tool shows.
4. **Artifact detection / compute check** pass at stock clocks. If you are comfortable doing it, an aggressive memory
   or core overclock should eventually make one of them (or the VRAM test) FAIL before games crash — tell me which.

### M5 — run modes
1. **Safety cutoff with real sensors**: `kiln-run --suite quick --max-temp 50` (any GPU that warms past 50 °C under
   load) must stop within a few seconds after the GPU passes 50 °C for 2 s, with the message
   "GPU temperature stayed above the 50 °C limit for 2 s (reached … °C)", exit code 4. Check the same in the GUI
   (Settings → Safety limits → 50 °C, then run Quick): the Live run page shows the message.
2. **Long loop**: GUI → Tests → Loop "Timed loop" 20m on the 4090. Watch the "Score per loop" chart; afterwards the
   Results → Loops tab should show a stability % and a trend (%/h). Send the results file.
3. **Device loss**: nothing safe can provoke a real driver reset. If one ever happens during any run, send the
   results file — `device_losses` holds the driver's fault report (NVIDIA and RADV implement
   VK_EXT_device_fault) — and say whether "Recover and continue" got the run going again.

### M4 — results, history, compare (GUI)
Use the GUI (`./Kiln-*.AppImage` / `kiln.exe`) after a few runs on the RTX 4090 and the iGPU:
1. **Sensors tab** (Results): temperature, power (with the power-limit line), clocks, load, fan and VRAM charts
   should show real NVML / amdgpu data; throttle periods shaded. Do the shaded periods match what you expect
   (e.g. power-limited during heavy load)? Screenshot the Sensors tab in your desktop's light and dark mode.
2. **History**: all runs appear; filter works; "Score history" shows comparable runs.
3. **Compare**: select a 4090 run and an iGPU run with the same settings (History → select two → Compare); check
   the per-test deltas and the frame-time overlay. Screenshot it.
4. **Export**: Export → HTML report; open it in a browser and print to PDF — does it look right on paper?
5. **Theme**: switch the desktop between light and dark (and change the KDE accent colour) while Kiln is open;
   all pages, charts and icons should follow without a restart.

### M2 — vertical slice (Vulkan runner, telemetry)
Run `hw-report.sh` on the CachyOS rig (RTX 4090 + 9950X3D iGPU) and `hw-report.ps1` on Windows if possible. Then check:
1. **Both adapters listed** (`kiln-run --list-devices`): the 4090 is the default; the Raphael iGPU (RADV) is listed too.
2. **NVIDIA telemetry (NVML)**: in `runs/vulkan-gpu0-quick.json` → `telemetry.samples` should have clocks, temperature,
   power, power limit, fan %, utilization, VRAM, and `throttle` flags when power-limited. `system.gpu.vbios` and
   `pcie_current`/`pcie_max` (e.g. "Gen4 x16") should be filled.
3. **AMD iGPU telemetry (amdgpu sysfs)**: for the iGPU run, `telemetry.provider` should be `amdgpu-sysfs`; check
   which channels are present (APUs expose fewer hwmon files than dGPUs) — tell me which are missing.
4. **Rendering**: `frames/*/scene.lattice_frame0000.png` should show coloured cubes over a checkered floor with a
   blue-grey sky (same image as CI's lavapipe reference). Any black/garbled frame is a bug.
5. **Window + HUD**: during the run a window shows the scene letterboxed with a HUD (test name, FPS, clocks/temps
   line). Press F1 to toggle the HUD, Esc to stop. Does it work on Wayland (KDE) and under X11
   (`SDL_VIDEO_DRIVER=x11`)? Is the present mode `immediate` or `mailbox` (see `logs/*-quick.log`, "swapchain" line)?
6. **Validation**: `logs/*-smoke.log` should contain no `[vulkan-validation]` errors (needs the Vulkan SDK or the
   distro's `vulkan-validation-layers` package installed; otherwise the run notes that validation was unavailable).

### M1 — packaging smoke
1. Linux (CachyOS/KDE): start the AppImage; a window titled "Kiln" must open. Run `./Kiln-*.AppImage run --version`.
2. Windows: unzip the portable zip; `kiln.exe` opens a window; `kiln-run.exe --version` prints the version.
Report: does it start on Wayland and on X11? Any missing-library errors?
