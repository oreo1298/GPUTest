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

## Pending checks
_(filled in as features land; see below)_

### M1 — packaging smoke
1. Linux (CachyOS/KDE): start the AppImage; a window titled "Kiln" must open. Run `./Kiln-*.AppImage run --version`.
2. Windows: unzip the portable zip; `kiln.exe` opens a window; `kiln-run.exe --version` prints the version.
Report: does it start on Wayland and on X11? Any missing-library errors?
