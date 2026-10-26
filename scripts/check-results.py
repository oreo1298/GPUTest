#!/usr/bin/env python3
"""Sanity-checks a results JSON file against the documented schema (docs/RESULTS_SCHEMA.md).

usage: check-results.py <results.json> [--require-completed] [--require-golden-pass]
Exit code 0 when OK; prints the problems otherwise. Standard library only.
"""
import json
import sys


def fail(msg):
    print("results check FAILED:", msg)
    sys.exit(1)


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        sys.exit(2)
    path = sys.argv[1]
    flags = set(sys.argv[2:])
    with open(path, encoding="utf-8") as f:
        r = json.load(f)
    if r.get("schema") != "gpu-benchmark-results":
        fail("schema id missing")
    if r.get("schema_version") != 1:
        fail(f"unexpected schema_version {r.get('schema_version')}")
    for key in ("app", "config", "system", "status", "tests", "loops", "scores", "telemetry", "validation"):
        if key not in r:
            fail(f"missing top-level key '{key}'")
    gpu = r["system"].get("gpu", {})
    for key in ("name", "vendor_id", "device_id", "driver_version", "api"):
        if key not in gpu:
            fail(f"system.gpu.{key} missing")
    if not r["tests"]:
        fail("no test results")
    for t in r["tests"]:
        for key in ("id", "name", "category", "status", "metrics", "duration_s"):
            if key not in t:
                fail(f"test missing '{key}': {t.get('id')}")
        if t["status"] == "completed" and "frame_stats" in t:
            fs = t["frame_stats"]
            if fs["frames"] <= 0 or fs["avg_fps"] <= 0:
                fail(f"{t['id']}: empty frame stats")
            if not (fs["p50_ms"] <= fs["p90_ms"] <= fs["p99_ms"] <= fs["p999_ms"] + 1e-9):
                fail(f"{t['id']}: percentiles not monotonic")
            if len(t.get("frame_times_ms", [])) != fs["frames"]:
                fail(f"{t['id']}: frame_times_ms length != frames")
    if "--require-completed" in flags:
        if r["status"] != "completed":
            fail(f"run status is {r['status']}: {r.get('status_message', '')}")
        bad = [t["id"] for t in r["tests"] if t["status"] not in ("completed", "skipped")]
        if bad:
            fail("tests not completed: " + ", ".join(bad))
    if r["validation"]["errors"]:
        fail(f"{r['validation']['errors']} validation errors")
    if "--require-golden-pass" in flags:
        for t in r["tests"]:
            if "golden" in t.get("details", {}) and t.get("verdict") == "fail":
                fail(f"{t['id']}: {t.get('verdict_message')}")
    print(f"results OK: {len(r['tests'])} test result(s), status {r['status']}, overall {r['scores']['overall']}")


if __name__ == "__main__":
    main()
