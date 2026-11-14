#!/bin/bash
# Wrap the SwiftUI shell as CoreVideo Pro.app (LaunchServices launch = proper
# key-window/keyboard routing + TCC usage strings). Ad-hoc signed by default.
#
# COREVIDEO_SIGN_IDENTITY="Developer ID Application: ..." (opt-in) signs with
# that identity instead: hardened runtime + timestamp + per-binary entitlements,
# nested code deepest-first. A stable identity stops TCC/Keychain re-prompting
# after every rebuild. Unset = ad-hoc, exactly as before (CI relies on this).
# Notarization: --notarize <keychain-profile> (needs the env var set).
# Release builds use scripts/release-macos.sh, which drives this script.
#
# Inputs (env, all optional; defaults are the dev build trees):
#   COREVIDEO_SHELL_BIN        shell executable   (mac-shell/.build/release/CoreVideoProShell)
#   COREVIDEO_CORE_BUILD_DIR   holds corevideo-native + corevideo-plugin-host (native/build-metal)
#   COREVIDEO_ENGINE_APP       corevideo-zoom-engine.app (native/build-engine/...)
#   COREVIDEO_BUILD_NUMBER     CFBundleVersion (default: git commit count)
#   COREVIDEO_REQUIRE_HELPERS=1  a missing core/engine/plugin-host is fatal
#                              (release); unset = skip it, as dev runs always did.
#
# Versioning: CFBundleShortVersionString = package.json "version" (the one
# product version); CFBundleVersion = monotonically increasing build number.
set -euo pipefail
cd "$(dirname "$0")/.."
NOTARIZE_PROFILE=""
while [ $# -gt 0 ]; do
    case "$1" in
        --notarize) NOTARIZE_PROFILE="${2:?--notarize needs a keychain profile}"; shift 2 ;;
        *) echo "error: unknown argument '$1'" >&2; exit 2 ;;
    esac
done
SIGN_ID="${COREVIDEO_SIGN_IDENTITY:--}"
REQUIRE_HELPERS="${COREVIDEO_REQUIRE_HELPERS:-0}"
if [ -n "$NOTARIZE_PROFILE" ] && [ "$SIGN_ID" = "-" ]; then
    echo "error: --notarize needs COREVIDEO_SIGN_IDENTITY (Apple rejects ad-hoc)" >&2
    exit 2
fi
. scripts/macos-sign-lib.sh
APP="mac-shell/.build/CoreVideo Pro.app"
BIN="${COREVIDEO_SHELL_BIN:-mac-shell/.build/release/CoreVideoProShell}"
CORE_DIR="${COREVIDEO_CORE_BUILD_DIR:-native/build-metal}"
ENGINE_SRC="${COREVIDEO_ENGINE_APP:-native/build-engine/corevideo-zoom-engine.app}"
[ -x "$BIN" ] || { echo "build the shell first (swift build -c release)"; exit 2; }
VERSION="$(node -p "require('./package.json').version" 2>/dev/null || true)"
[ -n "$VERSION" ] || VERSION="$(sed -n 's/^  "version": "\([^"]*\)".*/\1/p' package.json | head -1)"
[ -n "$VERSION" ] || { echo "error: no version in package.json" >&2; exit 2; }
BUILD_NUMBER="${COREVIDEO_BUILD_NUMBER:-$(git rev-list --count HEAD 2>/dev/null || echo 0)}"
case "$BUILD_NUMBER" in ''|*[!0-9]*) echo "error: build number '$BUILD_NUMBER' is not an integer" >&2; exit 2 ;; esac

# Every helper the app resolves from Contents/Resources (ShellPaths.resolve in
# AppModel.swift). Missing one in a release silently degrades a feature
# (no core = no app; no engine = no Zoom; no plugin host = VST "absent").
need_helper() {  # need_helper <path> <what>
    if [ ! -e "$1" ]; then
        if [ "$REQUIRE_HELPERS" = "1" ]; then
            echo "error: missing $2 at $1 (COREVIDEO_REQUIRE_HELPERS=1)" >&2; exit 2
        fi
        echo "warning: no $2 at $1; the bundle will not include it" >&2
        return 1
    fi
}
rm -rf "$APP"
mkdir -p "$APP/Contents/MacOS"
cp "$BIN" "$APP/Contents/MacOS/CoreVideoProShell"
cat > "$APP/Contents/Info.plist" <<PLIST
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>CFBundleExecutable</key><string>CoreVideoProShell</string>
    <key>CFBundleIdentifier</key><string>us.iamfatness.corevideopro.shell</string>
    <key>CFBundleName</key><string>CoreVideo Pro</string>
    <key>CFBundlePackageType</key><string>APPL</string>
    <key>CFBundleShortVersionString</key><string>$VERSION</string>
    <key>CFBundleVersion</key><string>$BUILD_NUMBER</string>
    <key>CFBundleInfoDictionaryVersion</key><string>6.0</string>
    <key>CFBundleDevelopmentRegion</key><string>en</string>
    <key>ATSApplicationFontsPath</key><string>Fonts</string>
    <key>CFBundleIconFile</key><string>AppIcon</string>
    <key>CFBundleURLTypes</key>
    <array><dict>
      <key>CFBundleURLName</key><string>us.iamfatness.corevideopro.oauth</string>
      <key>CFBundleURLSchemes</key><array><string>corevideo</string><string>corevideopro</string></array>
    </dict></array>
    <key>LSMinimumSystemVersion</key><string>13.0</string>
    <key>NSHighResolutionCapable</key><true/>
    <key>NSCameraUsageDescription</key><string>CoreVideo Pro captures cameras as production sources.</string>
    <key>NSMicrophoneUsageDescription</key><string>CoreVideo Pro captures audio inputs for the production mix.</string>
</dict>
</plist>
PLIST
# Self-contained: core + engine ride in Resources so the .app runs anywhere
# on this machine without the repo checkout (ShellPaths prefers Resources).
mkdir -p "$APP/Contents/Resources"
# Brand fonts (design handoff non-negotiable: Space Grotesk + IBM Plex Mono)
mkdir -p "$APP/Contents/Resources/Fonts"
cp mac-shell/Resources/Fonts/*.ttf "$APP/Contents/Resources/Fonts/"
# App icon (regenerate with scripts/make-app-icon.swift + iconutil)
cp mac-shell/Resources/AppIcon.icns "$APP/Contents/Resources/" 2>/dev/null || true
if need_helper "$CORE_DIR/corevideo-native" "media core"; then
  cp "$CORE_DIR/corevideo-native" "$APP/Contents/Resources/"
fi
# VST3 scan host: the core finds it ONLY via COREVIDEO_PLUGIN_HOST_PATH, which
# the shell sets from Resources (AppModel.swift, ShellPaths.pluginHostPath).
if need_helper "$CORE_DIR/corevideo-plugin-host" "VST3 plugin host"; then
  cp "$CORE_DIR/corevideo-plugin-host" "$APP/Contents/Resources/"
fi
# corevideo-browser-host is deliberately NOT bundled: the shell never resolves
# it and never sets COREVIDEO_BROWSER_HOST_PATH (the core's only lookup on
# macOS), and the UI states browser sources are unavailable on macOS.
if need_helper "$ENGINE_SRC" "Zoom engine app"; then
  # ditto, not cp -R: keeps the SDK frameworks' Versions/Current symlinks.
  ditto "$ENGINE_SRC" "$APP/Contents/Resources/corevideo-zoom-engine.app"
fi
if [ "$SIGN_ID" = "-" ]; then
    codesign --force --sign - "$APP"
else
    # Deepest first: engine SDK, engine app, core, shell main exe, then the app.
    ENG="$APP/Contents/Resources/corevideo-zoom-engine.app"
    if [ -d "$ENG" ]; then
        [ -L "$ENG/Contents/Frameworks" ] || [ ! -d "$ENG/Contents/Frameworks" ] || \
            sign_nested "$ENG/Contents/Frameworks"
        sign_one "$ENG" --entitlements scripts/macos-engine.entitlements
    fi
    # Bare executables get explicit identifiers (codesign would otherwise
    # use the file name) so TCC and notarization logs name them clearly.
    if [ -f "$APP/Contents/Resources/corevideo-native" ]; then
        sign_one "$APP/Contents/Resources/corevideo-native" \
            --identifier us.iamfatness.corevideopro.core \
            --entitlements scripts/macos-core.entitlements
    fi
    # The plugin host only walks the filesystem (no camera/mic, no foreign
    # code loaded in P1), so it gets no entitlements at all.
    if [ -f "$APP/Contents/Resources/corevideo-plugin-host" ]; then
        sign_one "$APP/Contents/Resources/corevideo-plugin-host" \
            --identifier us.iamfatness.corevideopro.plugin-host
    fi
    sign_one "$APP" --entitlements scripts/macos-shell.entitlements
    codesign --verify --deep --strict "$APP"
fi

if [ -n "$NOTARIZE_PROFILE" ]; then
    SUBMIT_ZIP="$(mktemp -d)/CoreVideoPro.zip"
    ditto -c -k --keepParent "$APP" "$SUBMIT_ZIP"
    xcrun notarytool submit "$SUBMIT_ZIP" --keychain-profile "$NOTARIZE_PROFILE" --wait
    xcrun stapler staple "$APP"
    xcrun stapler validate "$APP"
    rm -rf "$(dirname "$SUBMIT_ZIP")"
fi
echo "$APP"
