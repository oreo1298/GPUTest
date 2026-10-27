#!/usr/bin/env bash
# Provision build/test dependencies for a Claude Code cloud session (Ubuntu 24.04).
# Idempotent and safe to rerun. Always exits 0: a failed step is reported, never fatal.
#
# Installs: compilers + ccache, Mesa software drivers (lavapipe/llvmpipe), Vulkan
# loader/tools/validation layers, SDL3 build deps, Xvfb, Qt 6 (aqtinstall when
# download.qt.io is reachable, otherwise Ubuntu's Qt 6.4 as a compile-only fallback),
# and the Slang shader compiler (prebuilt GitHub release).

SLANG_VERSION="${SLANG_VERSION:-2026.20}"
QT_VERSION="${QT_VERSION:-6.8.3}"
QT_ROOT="/opt/qt"
SLANG_ROOT="/opt/slang"
LOG="${TMPDIR:-/tmp}/cloud-setup.log"

say() { echo "[cloud-setup] $*"; }
run() { "$@" >>"$LOG" 2>&1 || say "WARN: '$*' failed (see $LOG)"; }

: >"$LOG"
export DEBIAN_FRONTEND=noninteractive
SUDO=""
if [ "$(id -u)" != "0" ]; then SUDO="sudo"; fi

APT_PACKAGES=(
  build-essential cmake ninja-build ccache pkg-config git curl ca-certificates xz-utils unzip
  clang clang-format clang-tidy lld python3-pip
  mesa-vulkan-drivers libgl1-mesa-dri libegl-mesa0 libglx-mesa0 libvulkan-dev vulkan-tools vulkan-validationlayers
  spirv-tools glslang-tools mesa-utils
  libgl-dev libegl-dev libgles-dev libx11-dev libxext-dev libxrandr-dev libxcursor-dev
  libxfixes-dev libxi-dev libxss-dev libxtst-dev libxkbcommon-dev libwayland-dev
  wayland-protocols libdecor-0-dev libdrm-dev libgbm-dev libdbus-1-dev libudev-dev
  xvfb xauth x11-utils imagemagick file patchelf desktop-file-utils libfuse2t64
  libsqlite3-dev libfontconfig1-dev libxkbcommon-x11-0 libxcb-cursor0 libxcb-icccm4
  libxcb-image0 libxcb-keysyms1 libxcb-render-util0 libxcb-xinerama0 libxcb-xkb1
  fonts-noto-core fonts-dejavu-core
  g++-mingw-w64-x86-64-posix
)
QT_APT_PACKAGES=(qt6-base-dev qt6-base-dev-tools qt6-svg-dev libqt6sql6-sqlite qt6-wayland
  qt6-base-private-dev libqt6opengl6-dev qt6-tools-dev-tools)

missing=()
for p in "${APT_PACKAGES[@]}"; do
  dpkg -s "$p" >/dev/null 2>&1 || missing+=("$p")
done
if [ "${#missing[@]}" -gt 0 ]; then
  say "installing ${#missing[@]} apt packages"
  run $SUDO apt-get update
  run $SUDO apt-get install -y --no-install-recommends "${missing[@]}"
else
  say "apt packages already present"
fi

# ccache: shared cache, generous size, compression.
run ccache --max-size=8G
run ccache --set-config=compression=true

# --- Slang -----------------------------------------------------------------
if [ -x "$SLANG_ROOT/$SLANG_VERSION/bin/slangc" ]; then
  say "slang $SLANG_VERSION present"
else
  say "downloading slang $SLANG_VERSION"
  mkdir -p "$SLANG_ROOT/$SLANG_VERSION"
  url="https://github.com/shader-slang/slang/releases/download/v${SLANG_VERSION}/slang-${SLANG_VERSION}-linux-x86_64.tar.gz"
  if curl -fsSL --retry 3 -o "$SLANG_ROOT/slang.tar.gz" "$url"; then
    run tar -xzf "$SLANG_ROOT/slang.tar.gz" -C "$SLANG_ROOT/$SLANG_VERSION"
    rm -f "$SLANG_ROOT/slang.tar.gz"
  else
    say "WARN: slang download failed ($url)"
  fi
fi
[ -x "$SLANG_ROOT/$SLANG_VERSION/bin/slangc" ] && ln -sfn "$SLANG_ROOT/$SLANG_VERSION" "$SLANG_ROOT/current"

# --- Qt ----------------------------------------------------------------------
QT_PREFIX=""
if [ -f "$QT_ROOT/$QT_VERSION/gcc_64/lib/cmake/Qt6/Qt6Config.cmake" ]; then
  QT_PREFIX="$QT_ROOT/$QT_VERSION/gcc_64"
  say "Qt $QT_VERSION (aqt) present"
elif curl -fsS -o /dev/null --max-time 15 https://download.qt.io/online/qtsdkrepository/; then
  say "installing Qt $QT_VERSION via aqtinstall"
  run python3 -m pip install --break-system-packages --upgrade aqtinstall
  run python3 -m aqt install-qt linux desktop "$QT_VERSION" linux_gcc_64 -O "$QT_ROOT" -m qtwaylandcompositor
  [ -f "$QT_ROOT/$QT_VERSION/gcc_64/lib/cmake/Qt6/Qt6Config.cmake" ] && QT_PREFIX="$QT_ROOT/$QT_VERSION/gcc_64"
fi
if [ -z "$QT_PREFIX" ]; then
  say "download.qt.io unreachable or aqt failed: using Ubuntu Qt 6.4 (compile/test fallback only)"
  qmissing=()
  for p in "${QT_APT_PACKAGES[@]}"; do dpkg -s "$p" >/dev/null 2>&1 || qmissing+=("$p"); done
  if [ "${#qmissing[@]}" -gt 0 ]; then
    run $SUDO apt-get install -y --no-install-recommends "${qmissing[@]}"
  fi
fi

# --- Session environment ----------------------------------------------------------
if [ -n "${CLAUDE_ENV_FILE:-}" ]; then
  {
    echo "export PATH=\"$SLANG_ROOT/current/bin:\$PATH\""
    [ -n "$QT_PREFIX" ] && echo "export CMAKE_PREFIX_PATH=\"$QT_PREFIX\${CMAKE_PREFIX_PATH:+:\$CMAKE_PREFIX_PATH}\""
    echo "export CCACHE_DIR=\"\${CCACHE_DIR:-\$HOME/.cache/ccache}\""
  } >>"$CLAUDE_ENV_FILE"
fi

say "summary:"
say "  slangc: $("$SLANG_ROOT/current/bin/slangc" -v 2>&1 | head -1 || echo missing)"
say "  qt: ${QT_PREFIX:-apt $(dpkg -s qt6-base-dev 2>/dev/null | awk '/^Version/{print $2}')}"
say "  vulkan ICDs: $(ls /usr/share/vulkan/icd.d 2>/dev/null | tr '\n' ' ')"
say "  log: $LOG"
exit 0
