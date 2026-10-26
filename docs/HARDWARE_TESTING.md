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
