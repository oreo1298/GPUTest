#!/usr/bin/env bash
# Compile-checks the Direct3D 12 backend on Linux with mingw-w64 (syntax, types and warnings only;
# Windows binaries are built and run in CI). Uses the project's warning flags with -Werror.
#
#   scripts/check-d3d12.sh [build-dir] [file.cpp ...]
#
# build-dir (default build/gcc-debug) provides the generated gt/app_info.h. DirectX-Headers is
# cloned at the pinned tag into $GT_DEPS_CACHE (default build/deps-cache) when missing.
set -euo pipefail
root="$(cd "$(dirname "$0")/.." && pwd)"
build="${1:-$root/build/gcc-debug}"
[ $# -gt 0 ] && shift
cxx="${CXX_MINGW:-x86_64-w64-mingw32-g++-posix}"
command -v "$cxx" >/dev/null || { echo "$cxx not found (apt install g++-mingw-w64-x86-64-posix)" >&2; exit 2; }

deps="${GT_DEPS_CACHE:-$root/build/deps-cache}"
tag="$(sed -nE 's/^gt_declare\(directx_headers [^ ]+ ([^)]+)\)/\1/p' "$root/cmake/Dependencies.cmake")"
dxh="$deps/directx_headers"
if [ ! -f "$dxh/include/directx/d3d12.h" ]; then
    mkdir -p "$deps"
    git clone -q --depth 1 --branch "$tag" https://github.com/microsoft/DirectX-Headers.git "$dxh"
fi
sdl="$(ls -d "${GT_DEPS_CACHE:-/nonexistent}/sdl3" "$HOME/.cache/gt-deps/sdl3" "$build/_deps/sdl3-src" 2>/dev/null | head -1 || true)"
[ -n "$sdl" ] || { echo "SDL3 sources not found (configure $build first)" >&2; exit 2; }

files=("$@")
[ ${#files[@]} -gt 0 ] || files=("$root"/src/rhi/d3d12/*.cpp)
flags=(-std=c++20 -fsyntax-only -Wall -Wextra -Wpedantic -Wnon-virtual-dtor -Woverloaded-virtual -Wformat=2
       -Wimplicit-fallthrough -Wmissing-declarations -Wno-missing-field-initializers -Wno-dangling-reference -Werror
       -DGT_HAS_D3D12=1 -DNOMINMAX -DWIN32_LEAN_AND_MEAN -DUNICODE -D_UNICODE -D__REQUIRED_RPCNDR_H_VERSION__=475
       -I"$root/src/rhi" -I"$root/src/common" -I"$root/shaders/include" -I"$build/generated"
       -isystem "$dxh/include" -isystem "$sdl/include")
status=0
for f in "${files[@]}"; do
    echo "== $(basename "$f")"
    "$cxx" "${flags[@]}" "$f" || status=1
done
exit $status
