#!/usr/bin/env bash
# Pre-clone pinned third-party sources into a shared cache so several build
# directories (presets) don't each clone them. Usage:
#   GT_DEPS_CACHE=$HOME/.cache/gt-deps scripts/fetch-deps.sh
# then configure with the same GT_DEPS_CACHE in the environment.
set -euo pipefail
here="$(cd "$(dirname "$0")/.." && pwd)"
cache="${GT_DEPS_CACHE:-$HOME/.cache/gt-deps}"
mkdir -p "$cache"
grep -E '^gt_declare\(' "$here/cmake/Dependencies.cmake" | sed -E 's/gt_declare\(([^ ]+) ([^ ]+) ([^ )]+)\)/\1 \2 \3/' |
while read -r name url tag; do
  dir="$cache/$name"
  if [ -f "$dir/.gt-tag" ] && [ "$(cat "$dir/.gt-tag")" = "$tag" ]; then
    echo "[deps] $name $tag cached"; continue
  fi
  rm -rf "$dir"
  echo "[deps] cloning $name $tag"
  git -c advice.detachedHead=false clone --quiet --depth 1 --branch "$tag" "$url" "$dir"
  echo "$tag" > "$dir/.gt-tag"
done
echo "[deps] cache: $cache"
