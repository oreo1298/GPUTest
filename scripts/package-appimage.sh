#!/usr/bin/env bash
# Builds an AppImage from an existing CMake build directory.
#   scripts/package-appimage.sh <build-dir> [out-dir]
# Needs: qmake of the Qt the GUI was built with (QMAKE env or on PATH), network
# access to download linuxdeploy on first use (cached in <build-dir>/_appimage-tools).
set -euo pipefail
root="$(cd "$(dirname "$0")/.." && pwd)"
build="$(cd "${1:?usage: package-appimage.sh <build-dir> [out-dir]}" && pwd)"
out="${2:-$root/dist}"
mkdir -p "$out"
out="$(cd "$out" && pwd)"
eval "$("$root/scripts/app-info.sh")"
rev="$(git -C "$root" rev-parse --short=8 HEAD 2>/dev/null || echo local)"

tools="$build/_appimage-tools"
mkdir -p "$tools"
fetch() {
    [ -x "$tools/$1" ] || { curl -fsSL --retry 3 -o "$tools/$1" "$2" && chmod +x "$tools/$1"; }
}
fetch linuxdeploy-x86_64.AppImage https://github.com/linuxdeploy/linuxdeploy/releases/download/continuous/linuxdeploy-x86_64.AppImage
fetch linuxdeploy-plugin-qt-x86_64.AppImage https://github.com/linuxdeploy/linuxdeploy-plugin-qt/releases/download/continuous/linuxdeploy-plugin-qt-x86_64.AppImage
export PATH="$tools:$PATH"
export APPIMAGE_EXTRACT_AND_RUN=1
export QMAKE="${QMAKE:-$(command -v qmake6 || command -v qmake)}"
# Wayland client plugins are an add-on module in newer Qt packages; deploy them only
# when present (without them the image still runs everywhere through XWayland).
qt_plugins="$("$QMAKE" -query QT_INSTALL_PLUGINS)"
platform_plugins="libqoffscreen.so"
if [ -f "$qt_plugins/platforms/libqwayland-generic.so" ]; then
    platform_plugins="libqwayland-generic.so;libqwayland-egl.so;$platform_plugins"
    export EXTRA_QT_MODULES="waylandcompositor"
else
    echo "warning: Qt Wayland platform plugin not found in $qt_plugins; AppImage will use XWayland" >&2
fi
export EXTRA_PLATFORM_PLUGINS="$platform_plugins"
export DISABLE_COPYRIGHT_FILES_DEPLOYMENT=1

appdir="$(mktemp -d)/AppDir"
DESTDIR="$appdir" cmake --install "$build" --prefix /usr
sed -e "s/@APP_RUNNER_NAME@/$APP_RUNNER_NAME/g" -e "s/@APP_GUI_NAME@/$APP_GUI_NAME/g" \
    "$root/packaging/linux/AppRun" > "$build/_appimage-tools/AppRun"
chmod +x "$build/_appimage-tools/AppRun"
# Ship the hardware-report script and docs inside the image too.
install -Dm755 "$root/scripts/hw-report.sh" "$appdir/usr/share/$APP_ID/hw-report.sh" 2>/dev/null || true
install -Dm644 "$root/LICENSE" "$appdir/usr/share/doc/$APP_ID/LICENSE"
install -Dm644 "$root/THIRD_PARTY.md" "$appdir/usr/share/doc/$APP_ID/THIRD_PARTY.md" 2>/dev/null || true

export LDAI_OUTPUT="$out/${APP_DISPLAY_NAME}-${APP_VERSION}-${rev}-x86_64.AppImage"
cd "$out"
linuxdeploy-x86_64.AppImage --appdir "$appdir" \
    --desktop-file "$appdir/usr/share/applications/$APP_RDNN.desktop" \
    --icon-file "$appdir/usr/share/icons/hicolor/scalable/apps/$APP_RDNN.svg" \
    --executable "$appdir/usr/bin/$APP_GUI_NAME" \
    --executable "$appdir/usr/bin/$APP_RUNNER_NAME" \
    --custom-apprun "$build/_appimage-tools/AppRun" \
    --plugin qt --output appimage
echo "AppImage: $LDAI_OUTPUT"
