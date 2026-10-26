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

if [ -z "$quick" ] && command -v xvfb-run >/dev/null 2>&1; then
    step "windowed run under Xvfb (swapchain + HUD)"
    xvfb-run -a -s "-screen 0 1280x720x24" "$runner" --test scene.lattice --duration smoke --resolution 320x180 \
        --validation --fail-on-validation --out "$out/windowed.json" --log-file "$out/windowed.log"
    python3 "$root/scripts/check-results.py" "$out/windowed.json" --require-completed
    end
fi
echo "smoke OK"
