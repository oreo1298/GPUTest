#!/usr/bin/env bash
# Prints shell assignments for the values in cmake/AppInfo.cmake, e.g.
#   eval "$(scripts/app-info.sh)"; echo "$APP_ID"
here="$(cd "$(dirname "$0")/.." && pwd)"
sed -nE 's/^set\((APP_[A-Z_]+) "([^"]*)"\).*/\1="\2"/p' "$here/cmake/AppInfo.cmake"
