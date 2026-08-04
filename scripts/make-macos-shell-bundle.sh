#!/bin/bash
# Wrap the SwiftUI shell as CoreVideo Pro.app (LaunchServices launch = proper
# key-window/keyboard routing + TCC usage strings). Ad-hoc signed.
set -euo pipefail
cd "$(dirname "$0")/.."
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
    <key>LSMinimumSystemVersion</key><string>13.0</string>
    <key>NSHighResolutionCapable</key><true/>
    <key>NSCameraUsageDescription</key><string>CoreVideo Pro captures cameras as production sources.</string>
    <key>NSMicrophoneUsageDescription</key><string>CoreVideo Pro captures audio inputs for the production mix.</string>
</dict>
</plist>
PLIST
codesign --force --sign - "$APP"
echo "$APP"
