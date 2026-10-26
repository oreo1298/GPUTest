#!/usr/bin/env bash
# Turns the first error lines of CI logs into GitHub annotations, so failures can be
# read through the REST API (check-run annotations) without downloading full logs.
#   scripts/ci-annotate.sh <build-dir>
build="${1:-build}"
n=0
for f in "$build"/build.log "$build"/package.log "$build"/smoke/*.log "$build"/smoke/*.txt "$build"/Testing/Temporary/LastTest.log; do
    [ -f "$f" ] || continue
    grep -E "error[: ]|error C[0-9]+|FAILED|undefined reference|Assertion|AddressSanitizer|runtime error:|FAIL|check FAILED|Traceback|fatal:" "$f" 2>/dev/null |
        grep -vE "Werror|-Wno-|error_handler|ErrorActionPreference|errorOccurred|validity error" | head -n 25 |
        while IFS= read -r line; do
            line="${line//%/%25}"
            echo "::error title=$(basename "$f")::${line:0:900}"
        done
    n=$((n + 1))
done
echo "annotated from $n log file(s)"
