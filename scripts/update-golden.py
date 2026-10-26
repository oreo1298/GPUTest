#!/usr/bin/env python3
"""Creates/updates golden-frame references from a results file produced with --dump-frames.

usage: update-golden.py <results.json> <golden-dir> [--tolerance N]
Writes <golden-dir>/golden.json: {"tolerance": N, "tests": {test_id: {frame: {hash, signature}}}}.
Look at the dumped PNGs before committing new references.
"""
import json
import os
import sys


def main():
    if len(sys.argv) < 3:
        print(__doc__)
        sys.exit(2)
    results, outdir = sys.argv[1], sys.argv[2]
    tol = 8
    if "--tolerance" in sys.argv:
        tol = int(sys.argv[sys.argv.index("--tolerance") + 1])
    with open(results, encoding="utf-8") as f:
        r = json.load(f)
    path = os.path.join(outdir, "golden.json")
    golden = {"tolerance": tol, "tests": {}}
    if os.path.exists(path):
        with open(path, encoding="utf-8") as f:
            golden = json.load(f)
        golden["tolerance"] = tol
    gpu = r["system"]["gpu"]
    golden["device"] = f"{gpu['name']} / {gpu['driver_name']} {gpu['driver_version']}"
    golden["resolution"] = [r["config"]["width"], r["config"]["height"]]
    n = 0
    for t in r["tests"]:
        frames = t.get("details", {}).get("golden")
        if not frames:
            continue
        golden["tests"][t["id"]] = {k: {"hash": v["hash"], "signature": v["signature"]} for k, v in frames.items()}
        n += len(frames)
    os.makedirs(outdir, exist_ok=True)
    with open(path, "w", encoding="utf-8") as f:
        json.dump(golden, f, indent=2, sort_keys=True)
        f.write("\n")
    print(f"wrote {n} frame reference(s) to {path}")


if __name__ == "__main__":
    main()
