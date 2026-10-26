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

## The hardware report (start here)
- Linux: `./hw-report.sh` from the repo `scripts/` directory, or from inside the AppImage:
  `./Kiln-*.AppImage --appimage-extract >/dev/null && squashfs-root/usr/share/kiln/hw-report.sh --runner squashfs-root/usr/bin/kiln-run`
- Windows (portable zip folder): `powershell -ExecutionPolicy Bypass -File hw-report.ps1`
- Options: `--standard` / `-Mode standard` (longer), `--stress 20m` / `-Mode stress -Stress 20m`, `--api`, `--gpu`.
- It runs, for every GPU and API: `--sysinfo`, `--caps`, `--check-support`, the smoke suite with validation layers
  and frame dumps, then the quick suite with frame dumps. Output: `hwreport-<host>-<time>.tar.gz` / `.zip`.

## Pending checks

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
