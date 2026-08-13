#!/bin/bash
# Assemble corevideo-zoom-engine.app — the macOS Zoom engine bundle.
#
# The Zoom macOS SDK is not self-contained: at auth time it loads sibling
# runtime bundles (ssb_sdk, zNet, zPTUIEx, ...) through the MAIN bundle's
# Contents/Frameworks directory — not rpath, not relative to the framework.
# A bare engine binary therefore auths Failed(1) synchronously with no
# delegate callback. The engine must run as an .app with the SDK runtime in
# Contents/Frameworks; this script is the single place that knows that.
# (Hard-won in the CoreVideo OBS plugin mac port; see main-macos.mm's header.)
#
# Usage:
#   scripts/make-macos-engine-bundle.sh --build-dir native/build-engine [options]
#
#   --build-dir DIR   CMake build dir containing corevideo-zoom-engine (required)
#   --out DIR         Where to write the .app (default: <build-dir>)
#   --zoom-sdk DIR    Zoom macOS SDK runtime (default: $ZOOM_SDK_DIR, then
#                     ~/Developer/zoom-sdk-macos)
#   --link-sdk        Symlink the SDK instead of copying (~600MB). Fast for
#                     local iteration; NOT distributable.
#   --sign IDENTITY   codesign identity (default: "-", ad-hoc)
#
# Ad-hoc signing is fine for local runs. A distributable build needs a
# Developer ID identity plus notarization, which this script does not do.
# NEVER commit any Zoom SDK file to the repo.

set -euo pipefail

APP_NAME="corevideo-zoom-engine.app"
BUILD_DIR=""
OUT_DIR=""
ZOOM_SDK="${ZOOM_SDK_DIR:-$HOME/Developer/zoom-sdk-macos}"
LINK_SDK=0
SIGN_ID="-"

while [ $# -gt 0 ]; do
    case "$1" in
        --build-dir) BUILD_DIR="$2"; shift 2 ;;
        --out)       OUT_DIR="$2";   shift 2 ;;
        --zoom-sdk)  ZOOM_SDK="$2";  shift 2 ;;
        --sign)      SIGN_ID="$2";   shift 2 ;;
        --link-sdk)  LINK_SDK=1;     shift ;;
        -h|--help)   sed -n '2,26p' "$0"; exit 0 ;;
        *) echo "error: unknown argument '$1'" >&2; exit 2 ;;
    esac
done

[ -n "$BUILD_DIR" ] || { echo "error: --build-dir is required" >&2; exit 2; }
[ -f "$BUILD_DIR/corevideo-zoom-engine" ] || {
    echo "error: no corevideo-zoom-engine in '$BUILD_DIR' (build the" >&2
    echo "       corevideo-zoom-engine target first)" >&2; exit 2; }
OUT_DIR="${OUT_DIR:-$BUILD_DIR}"
SRC_DIR="$(cd "$(dirname "$0")/.." && pwd)"
VERSION="$(node -p "require('$SRC_DIR/package.json').version" 2>/dev/null || echo 0.0.0)"

APP="$OUT_DIR/$APP_NAME"
rm -rf "$APP"
mkdir -p "$APP/Contents/MacOS"
cp "$BUILD_DIR/corevideo-zoom-engine" "$APP/Contents/MacOS/corevideo-zoom-engine"

cat > "$APP/Contents/Info.plist" <<PLIST
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>CFBundleDevelopmentRegion</key><string>en</string>
    <key>CFBundleExecutable</key><string>corevideo-zoom-engine</string>
    <key>CFBundleIdentifier</key><string>us.iamfatness.corevideopro.zoom-engine</string>
    <key>CFBundleInfoDictionaryVersion</key><string>6.0</string>
    <key>CFBundleName</key><string>corevideo-zoom-engine</string>
    <key>CFBundlePackageType</key><string>APPL</string>
    <key>CFBundleShortVersionString</key><string>$VERSION</string>
    <key>CFBundleVersion</key><string>$VERSION</string>
    <key>LSMinimumSystemVersion</key><string>12.0</string>
    <key>NSCameraUsageDescription</key><string>CoreVideo Pro captures Zoom meeting video.</string>
    <key>NSMicrophoneUsageDescription</key><string>CoreVideo Pro captures Zoom meeting audio.</string>
</dict>
</plist>
PLIST

if [ -d "$ZOOM_SDK/ZoomSDK.framework" ]; then
    if [ "$LINK_SDK" -eq 1 ]; then
        ln -s "$ZOOM_SDK" "$APP/Contents/Frameworks"
        echo "note: engine SDK is a symlink to $ZOOM_SDK (dev only, not distributable)"
    else
        ditto "$ZOOM_SDK" "$APP/Contents/Frameworks"
    fi
else
    echo "warning: no ZoomSDK.framework under '$ZOOM_SDK'; the engine will" >&2
    echo "         report sdk_runtime_missing and cannot authenticate." >&2
    echo "         Pass --zoom-sdk DIR or set ZOOM_SDK_DIR." >&2
fi

# The build-tree binary carries an absolute rpath to this machine's SDK
# checkout. Leave it and the shipped engine silently prefers that path over
# its own bundled copy — the bundle would be untested everywhere but here.
if [ ! -L "$APP/Contents/Frameworks" ]; then
    while :; do
        rp="$(otool -l "$APP/Contents/MacOS/corevideo-zoom-engine" \
              | awk '/LC_RPATH/{f=1} f&&/path /{print $2; exit}')"
        [ -n "$rp" ] || break
        install_name_tool -delete_rpath "$rp" \
            "$APP/Contents/MacOS/corevideo-zoom-engine" 2>/dev/null || break
    done
    install_name_tool -add_rpath "@executable_path/../Frameworks" \
        "$APP/Contents/MacOS/corevideo-zoom-engine"
fi

codesign --force --deep --sign "$SIGN_ID" "$APP" 2>/dev/null || \
    codesign --force --sign "$SIGN_ID" "$APP"

echo "bundle: $APP"
echo "engine binary: $APP/Contents/MacOS/corevideo-zoom-engine"
echo "Point the core at it with COREVIDEO_ZOOM_ENGINE_PATH=$APP/Contents/MacOS/corevideo-zoom-engine"
