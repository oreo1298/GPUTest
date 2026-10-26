#!/usr/bin/env bash
# Hardware report: runs a validation suite on every GPU and bundles results JSON,
# logs, system information and rendered frames ("screenshots") into one archive.
#
#   ./hw-report.sh [--runner PATH] [--quick|--standard|--stress 20m] [--gpu N] [--api vulkan] [--out DIR]
#
# Run it from the extracted AppImage directory, the build tree, or with the AppImage:
#   ./Kiln-*.AppImage run --version          # check the runner works
#   APPIMAGE_RUNNER="./Kiln-*.AppImage run" ./hw-report.sh
# Then commit the .tar.gz to hardware-reports/ or attach it to the session.
set -uo pipefail

mode="quick"
stress=""
only_gpu=""
only_api=""
outdir="$PWD"
runner_cmd="${APPIMAGE_RUNNER:-}"
while [ $# -gt 0 ]; do
    case "$1" in
        --runner) runner_cmd="$2"; shift 2 ;;
        --quick) mode="quick"; shift ;;
        --standard) mode="standard"; shift ;;
        --stress) mode="stress"; stress="${2:-20m}"; shift 2 ;;
        --gpu) only_gpu="$2"; shift 2 ;;
        --api) only_api="$2"; shift 2 ;;
        --out) outdir="$2"; shift 2 ;;
        -h|--help) sed -n '2,12p' "$0"; exit 0 ;;
        *) echo "unknown option $1"; exit 2 ;;
    esac
done

here="$(cd "$(dirname "$0")" && pwd)"
if [ -z "$runner_cmd" ]; then
    for c in "$here"/*-run "$here"/../bin/*-run "$here"/../../bin/*-run "$here"/../build/*/bin/*-run; do
        [ -x "$c" ] && { runner_cmd="$c"; break; }
    done
fi
if [ -z "$runner_cmd" ]; then
    runner_cmd="$(command -v kiln-run || true)"
fi
if [ -z "$runner_cmd" ]; then
    echo "Cannot find the benchmark runner (*-run). Pass --runner PATH." >&2
    exit 1
fi
read -r -a RUNNER <<< "$runner_cmd"
echo "runner: ${RUNNER[*]}"

stamp="$(date +%Y%m%d-%H%M%S)"
host="$(hostname 2>/dev/null || echo host)"
name="hwreport-${host}-${stamp}"
work="$(mktemp -d)/$name"
mkdir -p "$work"/{system,runs,frames,logs}
echo "collecting into $work"

capture() { # capture <file> <command...>
    local f="$1"; shift
    if command -v "$1" >/dev/null 2>&1; then "$@" > "$work/system/$f" 2>&1 || true; fi
}
{
    echo "date: $(date -Is)"
    echo "kernel: $(uname -a)"
    [ -f /etc/os-release ] && cat /etc/os-release
    echo "session: ${XDG_SESSION_TYPE:-?} desktop: ${XDG_CURRENT_DESKTOP:-?} wayland: ${WAYLAND_DISPLAY:-} display: ${DISPLAY:-}"
} > "$work/system/os.txt"
capture lscpu.txt lscpu
capture memory.txt free -h
capture lspci.txt lspci -nnk
capture lspci-vga-verbose.txt sh -c 'for d in $(lspci -D | grep -Ei "vga|3d|display" | cut -d" " -f1); do lspci -vvv -s "$d"; done'
capture vulkaninfo.txt vulkaninfo --summary
capture glxinfo.txt glxinfo -B
capture nvidia-smi.txt nvidia-smi -q
capture sensors.txt sensors
capture dmesg-gpu.txt sh -c 'dmesg 2>/dev/null | grep -Ei "amdgpu|nvidia|nouveau|i915|xe |drm|gpu" | tail -n 300'
capture kernel-gpu-log.txt sh -c 'journalctl -k -b --no-pager 2>/dev/null | grep -Ei "amdgpu|nvidia|nouveau|i915|xe |drm|gpu" | tail -n 300'
for f in /sys/class/drm/card*/device; do
    [ -d "$f" ] || continue
    c="$(basename "$(dirname "$f")")"
    {
        for k in vendor device subsystem_vendor subsystem_device current_link_speed current_link_width max_link_speed max_link_width vbios_version power_dpm_force_performance_level; do
            [ -r "$f/$k" ] && echo "$k: $(cat "$f/$k" 2>/dev/null)"
        done
        ls "$f/hwmon" 2>/dev/null | while read -r h; do
            for s in "$f/hwmon/$h"/*_input "$f/hwmon/$h"/*_label; do [ -r "$s" ] && echo "$h/$(basename "$s"): $(cat "$s" 2>/dev/null)"; done
        done
    } > "$work/system/sysfs-$c.txt" 2>/dev/null
done

"${RUNNER[@]}" --version > "$work/system/runner-version.txt" 2>&1
"${RUNNER[@]}" --list-devices --json > "$work/system/devices.json" 2> "$work/logs/list-devices.log"
"${RUNNER[@]}" --list-tests --json > "$work/system/tests.json" 2>/dev/null

# Iterate (api, gpu) pairs from the device list; software rasterizers are skipped unless they are all there is.
pairs="$(python3 - "$work/system/devices.json" "$only_api" "$only_gpu" <<'PY' 2>/dev/null
import json, sys
d = json.load(open(sys.argv[1]))
api_filter, gpu_filter = sys.argv[2], sys.argv[3]
out = []
for api in d.get("apis", []):
    if api_filter and api["api"] != api_filter:
        continue
    adapters = api.get("adapters", [])
    hw = [a for a in adapters if not a.get("software")] or adapters
    for a in hw:
        if gpu_filter and str(a["index"]) != gpu_filter:
            continue
        out.append(f"{api['api']} {a['index']}")
print("\n".join(out))
PY
)"
if [ -z "$pairs" ]; then
    echo "no GPUs found by the runner; see $work/logs/list-devices.log"
fi

while read -r api gpu; do
    [ -n "${api:-}" ] || continue
    tag="${api}-gpu${gpu}"
    echo "== $tag"
    "${RUNNER[@]}" --api "$api" --gpu "$gpu" --sysinfo > "$work/system/sysinfo-$tag.json" 2> "$work/logs/sysinfo-$tag.log"
    "${RUNNER[@]}" --api "$api" --gpu "$gpu" --caps --json > "$work/system/caps-$tag.json" 2> "$work/logs/caps-$tag.log"
    "${RUNNER[@]}" --api "$api" --gpu "$gpu" --check-support > "$work/system/support-$tag.txt" 2>&1
    # 1) correctness: smoke suite with validation layers and frame dumps
    "${RUNNER[@]}" --api "$api" --gpu "$gpu" --suite smoke --validation --resolution 1280x720 \
        --dump-frames "$work/frames/$tag-smoke" --out "$work/runs/$tag-smoke.json" \
        --log-file "$work/logs/$tag-smoke.log" --progress jsonl > "$work/logs/$tag-smoke.jsonl" 2>&1
    echo "   smoke exit code $?" | tee -a "$work/logs/exit-codes.txt"
    # 2) performance: quick / standard suite, or a timed stress loop
    case "$mode" in
        quick) args=(--suite quick) ;;
        standard) args=(--suite standard) ;;
        stress) args=(--suite quick --loop "$stress") ;;
    esac
    "${RUNNER[@]}" --api "$api" --gpu "$gpu" "${args[@]}" --dump-frames "$work/frames/$tag-$mode" \
        --out "$work/runs/$tag-$mode.json" --log-file "$work/logs/$tag-$mode.log" \
        --progress jsonl > "$work/logs/$tag-$mode.jsonl" 2>&1
    echo "   $mode exit code $?" | tee -a "$work/logs/exit-codes.txt"
done <<< "$pairs"

archive="$outdir/$name.tar.gz"
tar -czf "$archive" -C "$(dirname "$work")" "$name"
echo
echo "Report written to: $archive"
echo "Commit it to hardware-reports/ (or attach it to the session)."
