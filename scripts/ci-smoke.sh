#!/usr/bin/env bash
# Smoke test on Mesa lavapipe: runs the runner's information commands, the smoke
# suite offscreen with validation + golden-frame comparison, and (when Xvfb is
# available) a short windowed run to exercise the swapchain path.
#   scripts/ci-smoke.sh <build-dir> [--quick]
set -euo pipefail
root="$(cd "$(dirname "$0")/.." && pwd)"
build="$(cd "${1:?usage: ci-smoke.sh <build-dir>}" && pwd)"
quick="${2:-}"
eval "$("$root/scripts/app-info.sh")"
runner="$build/bin/$APP_RUNNER_NAME"
out="$build/smoke"
rm -rf "$out" && mkdir -p "$out"
export VK_DRIVER_FILES="${VK_DRIVER_FILES:-/usr/share/vulkan/icd.d/lvp_icd.json}"
export LP_NUM_THREADS="${LP_NUM_THREADS:-4}"

step() { echo "::group::$*" 2>/dev/null || true; echo "== $*"; }
end() { echo "::endgroup::" 2>/dev/null || true; }

step "version / devices / tests"
"$runner" --version
"$runner" --list-devices --json | tee "$out/devices.json" | python3 -c 'import json,sys; d=json.load(sys.stdin); assert d["apis"], "no APIs"'
"$runner" --list-tests --json > "$out/tests.json"
"$runner" --list-suites --json > "$out/suites.json"
"$runner" --sysinfo > "$out/sysinfo.json"
"$runner" --check-support > "$out/support.txt"
"$runner" --caps --json > "$out/caps.json"
python3 -c 'import json,sys; c=json.load(open(sys.argv[1])); print(len(c), "capability rows"); assert len(c) > 100' "$out/caps.json"
end

step "smoke suite (offscreen, validation, golden frames)"
"$runner" --suite smoke --offscreen --resolution 320x180 --validation --fail-on-validation \
    --dump-frames "$out/frames" --golden "$root/tests/golden/lavapipe" \
    --out "$out/smoke.json" --log-file "$out/smoke.log" --progress jsonl > "$out/smoke.jsonl"
python3 "$root/scripts/check-results.py" "$out/smoke.json" --require-completed --require-golden-pass
python3 - "$out/smoke.jsonl" <<'PY'
import json, sys
types = [json.loads(l)["type"] for l in open(sys.argv[1]) if l.strip()]
for t in ("hello", "run_start", "test_start", "test_end", "run_end"):
    assert t in types, f"missing {t} event"
print(len(types), "events OK")
PY
end

# Expect a given exit code from a command (set -e would abort on the non-zero ones).
expect_exit() {
    local want="$1"; shift
    local got=0
    "$@" || got=$?
    if [ "$got" != "$want" ]; then echo "expected exit code $want, got $got: $*"; exit 1; fi
}
# Checks fields of a results file: check_json FILE 'python expression over r'
check_json() {
    python3 - "$1" "$2" <<'PY'
import json, sys
r = json.load(open(sys.argv[1]))
ok = eval(sys.argv[2], {"r": r})
if not ok:
    print("check failed:", sys.argv[2])
    print(json.dumps({k: r.get(k) for k in ("status", "status_message", "device_losses")}, indent=1))
    sys.exit(1)
print("ok:", sys.argv[2])
PY
}

step "OpenGL (llvmpipe): standard suite; scenes against the Vulkan golden frames"
"$runner" --api opengl --suite standard --duration smoke --quality low --offscreen --resolution 320x180 \
    --validation --fail-on-validation --dump-frames "$out/gl-frames" --golden "$root/tests/golden/lavapipe" \
    --out "$out/gl-standard.json" --log-file "$out/gl-standard.log"
python3 "$root/scripts/check-results.py" "$out/gl-standard.json" --require-completed --require-golden-pass
check_json "$out/gl-standard.json" 'sum(1 for t in r["tests"] if "golden" in t.get("details", {})) == 5'
check_json "$out/gl-standard.json" 'all(t["status"] in ("completed", "skipped") for t in r["tests"])'
end

step "every scene: golden frames (offscreen, validation)"
"$runner" --test scene.courtyard,scene.courtyard.lite,scene.atrium,scene.atrium.lite,scene.skyline,scene.glasshouse,scene.gallery \
    --duration smoke --quality low --offscreen --resolution 320x180 --validation --fail-on-validation \
    --dump-frames "$out/scene-frames" --golden "$root/tests/golden/lavapipe" --out "$out/scenes.json"
python3 "$root/scripts/check-results.py" "$out/scenes.json" --require-completed --require-golden-pass
check_json "$out/scenes.json" 'sum(1 for t in r["tests"] if "golden" in t["details"]) == 7'
end

step "run modes: safety cutoff (fake sensors), device loss (abort / recover), stop, timed loop"
common=(--offscreen --resolution 320x180 --progress quiet)
GT_FAKE_TELEMETRY=ramp:10 expect_exit 4 "$runner" --test scene.lattice --duration quick --max-temp 60 "${common[@]}" \
    --out "$out/safety.json"
check_json "$out/safety.json" 'r["status"] == "safety_abort" and "stayed above the 60" in r["status_message"] and r["telemetry"]["provider"] == "fake"'
expect_exit 5 "$runner" --test debug.device_lost,scene.lattice --duration smoke "${common[@]}" --out "$out/lost.json"
check_json "$out/lost.json" 'r["status"] == "device_lost" and [t["status"] for t in r["tests"]] == ["device_lost"] and len(r["device_losses"]) == 1 and not r["device_losses"][0]["recovered"]'
expect_exit 5 "$runner" --test debug.device_lost,scene.lattice --duration smoke --on-device-lost recover "${common[@]}" \
    --out "$out/lost-recover.json"
check_json "$out/lost-recover.json" 'r["status"] == "completed" and [t["status"] for t in r["tests"]] == ["device_lost", "completed"] and r["device_losses"][0]["recovered"]'
( sleep 3; echo stop ) | expect_exit 3 "$runner" --suite smoke --duration smoke --loop inf --control-stdin "${common[@]}" \
    --out "$out/stop.json"
check_json "$out/stop.json" 'r["status"] == "stopped" and len(r["loops"]) >= 2'
expect_exit 0 "$runner" --suite smoke --duration smoke --loop 4s "${common[@]}" --out "$out/timed.json"
check_json "$out/timed.json" 'r["status"] == "completed" and len(r["loops"]) >= 2 and r["duration_s"] >= 4'
end

step "standard suite at smoke size (scenes, synthetic and feature tests) with validation"
# SER and opacity micromaps are not in lavapipe: their baseline halves still run.
"$runner" --suite standard --duration smoke --quality low --validation --fail-on-validation \
    --param feature.rt.ser.baseline_only=1 --param feature.rt.omm.baseline_only=1 "${common[@]}" --out "$out/standard.json"
python3 "$root/scripts/check-results.py" "$out/standard.json" --require-completed
check_json "$out/standard.json" 'sum(1 for t in r["tests"] if t["category"] == "synthetic") >= 10'
check_json "$out/standard.json" 'sum(1 for t in r["tests"] if t["category"] == "feature" and t["status"] == "completed") >= 10'
check_json "$out/standard.json" 'all(t.get("verdict") == "pass" for t in r["tests"] if t["category"] == "feature" and t["status"] == "completed")'
# The BVH probe check must catch a structure that was not refit after the geometry moved.
expect_exit 7 "$runner" --test feature.rt.bvh --duration smoke --param feature.rt.bvh.inject=1 "${common[@]}" \
    --out "$out/bvh-bad.json"
check_json "$out/bvh-bad.json" 'r["tests"][0]["verdict"] == "fail"'
# Upscaled scenes render below the output size; FSR 1's passes must validate too.
"$runner" --test scene.lattice,scene.courtyard --duration smoke --quality low --upscale fsr1 --validation \
    --fail-on-validation "${common[@]}" --out "$out/upscaled.json"
python3 "$root/scripts/check-results.py" "$out/upscaled.json" --require-completed
check_json "$out/upscaled.json" 'r["config"]["upscale"] == "fsr1-quality" and all(t["details"].get("upscaler") == "fsr1 quality" for t in r["tests"])'
end

step "diagnostics: clean run with validation, and injected faults must be detected"
"$runner" --suite diagnostics --duration smoke --validation --fail-on-validation "${common[@]}" --out "$out/diag.json"
check_json "$out/diag.json" 'all(t.get("verdict") == "pass" or t["status"] == "skipped" for t in r["tests"]) and len(r["tests"]) == 5'
for t in vram artifacts compute; do
    expect_exit 7 "$runner" --test "diag.$t" --duration smoke --param "diag.$t.inject=1" "${common[@]}" --out "$out/diag-$t-bad.json"
    check_json "$out/diag-$t-bad.json" 'r["tests"][0]["verdict"] == "fail"'
done
expect_exit 0 "$runner" --test diag.pcie --duration smoke --param diag.pcie.force=1 --validation --fail-on-validation \
    "${common[@]}" --out "$out/diag-pcie.json"
check_json "$out/diag-pcie.json" 'r["tests"][0]["verdict"] == "pass" and r["tests"][0]["metrics"][0]["value"] > 0'
GT_FAKE_TELEMETRY=ramp:8 expect_exit 0 "$runner" --test diag.thermal --duration smoke --param diag.soak_s=6 --no-safety \
    "${common[@]}" --out "$out/diag-soak.json"
check_json "$out/diag-soak.json" 'r["tests"][0]["verdict"] == "warn" and "thermal throttling" in r["tests"][0]["verdict_message"]'
end

if [ -z "$quick" ] && command -v xvfb-run >/dev/null 2>&1; then
    step "windowed run under Xvfb (swapchain + HUD)"
    xvfb-run -a -s "-screen 0 1280x720x24" "$runner" --test scene.lattice --duration smoke --resolution 320x180 \
        --validation --fail-on-validation --out "$out/windowed.json" --log-file "$out/windowed.log"
    python3 "$root/scripts/check-results.py" "$out/windowed.json" --require-completed
    end
fi
echo "smoke OK"
