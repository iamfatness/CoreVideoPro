#!/bin/bash
# Wrap the SwiftUI shell as CoreVideo Pro.app (LaunchServices launch = proper
# key-window/keyboard routing + TCC usage strings). Ad-hoc signed by default.
#
# COREVIDEO_SIGN_IDENTITY="Developer ID Application: ..." (opt-in) signs with
# that identity instead: hardened runtime + timestamp + per-binary entitlements,
# nested code deepest-first. A stable identity stops TCC/Keychain re-prompting
# after every rebuild. Unset = ad-hoc, exactly as before (CI relies on this).
# Notarization: --notarize <keychain-profile> (needs the env var set).
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
if [ -n "$NOTARIZE_PROFILE" ] && [ "$SIGN_ID" = "-" ]; then
    echo "error: --notarize needs COREVIDEO_SIGN_IDENTITY (Apple rejects ad-hoc)" >&2
    exit 2
fi
. scripts/macos-sign-lib.sh
APP="mac-shell/.build/CoreVideo Pro.app"
BIN="mac-shell/.build/release/CoreVideoProShell"
[ -x "$BIN" ] || { echo "build the shell first (swift build -c release)"; exit 2; }
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
    <key>CFBundleShortVersionString</key><string>0.1.0</string>
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
if [ -x native/build-metal/corevideo-native ]; then
  cp native/build-metal/corevideo-native "$APP/Contents/Resources/"
fi
if [ -d native/build-engine/corevideo-zoom-engine.app ]; then
  rm -rf "$APP/Contents/Resources/corevideo-zoom-engine.app"
  cp -R native/build-engine/corevideo-zoom-engine.app "$APP/Contents/Resources/" 2>/dev/null || true
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
    [ -f "$APP/Contents/Resources/corevideo-native" ] && \
        sign_one "$APP/Contents/Resources/corevideo-native" \
            --entitlements scripts/macos-core.entitlements
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
