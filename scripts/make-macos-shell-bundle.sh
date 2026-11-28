#!/bin/bash
# Wrap the SwiftUI shell as CoreVideo Pro.app (LaunchServices launch = proper
# key-window/keyboard routing + TCC usage strings). Ad-hoc signed by default.
#
# COREVIDEO_SIGN_IDENTITY="Developer ID Application: ..." (opt-in) signs with
# that identity instead: hardened runtime + timestamp + per-binary entitlements,
# nested code deepest-first. A stable identity stops TCC/Keychain re-prompting
# after every rebuild. Unset = ad-hoc, exactly as before (CI relies on this).
# Notarization: --notarize <keychain-profile> (needs the env var set), e.g.
# --notarize "CoreVideo Pro" (CoreVideo Pro's own profile; "CoreVideo Plugin" is the OBS plugin's).
# Release builds use scripts/release-macos.sh, which drives this script.
#
# Inputs (env, all optional; defaults are the dev build trees):
#   COREVIDEO_SHELL_BIN        shell executable   (mac-shell/.build/release/CoreVideoProShell)
#   COREVIDEO_CAMERA_EXT_BIN   camera extension executable (mac-shell/.build/release/CoreVideoProCamera)
#   COREVIDEO_APP_PROFILE      Developer ID profile for the app
#                              (~/Library/Developer/CoreVideoPro/profiles/CoreVideo_Pro_macOS_Developer_ID.provisionprofile)
#   COREVIDEO_CAMERA_PROFILE   Developer ID profile for the camera extension
#                              (~/Library/Developer/CoreVideoPro/profiles/CoreVideo_Pro_Camera_Extension_Developer_ID.provisionprofile)
#                              Both profiles are REQUIRED for a Developer ID build and are
#                              never committed. Ad-hoc builds use neither.
#   COREVIDEO_CORE_BUILD_DIR   holds corevideo-native + corevideo-plugin-host (native/build-metal)
#   COREVIDEO_ENGINE_APP       corevideo-zoom-engine.app (native/build-engine/...)
#   COREVIDEO_BUILD_NUMBER     CFBundleVersion (default: git commit count)
#   COREVIDEO_APP_OUT          where to assemble the .app (mac-shell/.build/CoreVideo Pro.app);
#                              release-macos.sh points this into its own work dir so a
#                              release never overwrites the developer's dev bundle
#   COREVIDEO_FFMPEG_BUILT_DIR output dir of scripts/build-macos-ffmpeg.sh (holds bin/ffmpeg
#                              and notices/ffmpeg/); its ffmpeg ships at Contents/Resources/ffmpeg,
#                              where the core finds it first (PosixExecutableLocator.h)
#   COREVIDEO_REQUIRE_HELPERS=1  a missing core/engine/plugin-host/child-guard/ffmpeg is fatal
#                              (release); unset = skip it, as dev runs always did.
#
# Camera extension (roadmap M9.2): the CoreMediaIO system extension rides at
# Contents/Library/SystemExtensions/us.iamfatness.corevideopro.shell.camera.systemextension
# with CFBundleVersion = CFBundleShortVersionString = CameraConstants.extensionVersion
# (its OWN version, never the app's build number: macOS replaces the installed
# extension whenever it changes, and a replacement can need a restart; see
# scripts/camera-extension-version.sh). scripts/check-macos-camera-extension.sh
# checks the result. Ad-hoc: macos-camera-extension-adhoc.entitlements and the
# shell on macos-shell.entitlements (no restricted entitlement anywhere: AMFI
# kills a binary signed with one and no profile). Developer ID: both profiles
# embedded, the extension on macos-camera-extension.entitlements and the shell
# on macos-shell-sysext.entitlements.
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
APP="${COREVIDEO_APP_OUT:-mac-shell/.build/CoreVideo Pro.app}"
mkdir -p "$(dirname "$APP")"
BIN="${COREVIDEO_SHELL_BIN:-mac-shell/.build/release/CoreVideoProShell}"
CORE_DIR="${COREVIDEO_CORE_BUILD_DIR:-native/build-metal}"
ENGINE_SRC="${COREVIDEO_ENGINE_APP:-native/build-engine/corevideo-zoom-engine.app}"
CAMERA_BIN="${COREVIDEO_CAMERA_EXT_BIN:-mac-shell/.build/release/CoreVideoProCamera}"
APP_PROFILE="${COREVIDEO_APP_PROFILE:-$HOME/Library/Developer/CoreVideoPro/profiles/CoreVideo_Pro_macOS_Developer_ID.provisionprofile}"
CAMERA_PROFILE="${COREVIDEO_CAMERA_PROFILE:-$HOME/Library/Developer/CoreVideoPro/profiles/CoreVideo_Pro_Camera_Extension_Developer_ID.provisionprofile}"
EXT_ID="us.iamfatness.corevideopro.shell.camera"
[ -x "$BIN" ] || { echo "build the shell first (swift build -c release)"; exit 2; }
[ -x "$CAMERA_BIN" ] || { echo "error: no camera extension at $CAMERA_BIN (swift build -c release builds it)" >&2; exit 2; }
if [ "$SIGN_ID" != "-" ]; then
    # A Developer ID build without its profiles would sign restricted
    # entitlements nothing authorizes: AMFI kills the app at launch.
    for p in "COREVIDEO_APP_PROFILE|$APP_PROFILE" "COREVIDEO_CAMERA_PROFILE|$CAMERA_PROFILE"; do
        [ -f "${p#*|}" ] || {
            echo "error: Developer ID signing needs both provisioning profiles (COREVIDEO_APP_PROFILE, COREVIDEO_CAMERA_PROFILE); ${p%%|*} -> ${p#*|} does not exist" >&2
            exit 2
        }
    done
    verify_profile "$APP_PROFILE" "42394L3353.us.iamfatness.corevideopro.shell" 0 || exit 2
    verify_profile "$CAMERA_PROFILE" "42394L3353.$EXT_ID" 0 || exit 2
fi
VERSION="$(node -p "require('./package.json').version" 2>/dev/null || true)"
[ -n "$VERSION" ] || VERSION="$(sed -n 's/^  "version": "\([^"]*\)".*/\1/p' package.json | head -1)"
[ -n "$VERSION" ] || { echo "error: no version in package.json" >&2; exit 2; }
BUILD_NUMBER="${COREVIDEO_BUILD_NUMBER:-$(git rev-list --count HEAD 2>/dev/null || echo 0)}"
case "$BUILD_NUMBER" in ''|*[!0-9]*) echo "error: build number '$BUILD_NUMBER' is not an integer" >&2; exit 2 ;; esac
EXT_VERSION="$(scripts/camera-extension-version.sh version)" \
    || { echo "error: no CameraConstants.extensionVersion" >&2; exit 2; }

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
    <key>NSRemovableVolumesUsageDescription</key><string>CoreVideo Pro saves recordings to the folder you choose, including external drives.</string>
</dict>
</plist>
PLIST
# Self-contained: core + engine ride in Resources so the .app runs anywhere
# on this machine without the repo checkout (ShellPaths prefers Resources).
mkdir -p "$APP/Contents/Resources"
# Brand fonts (design handoff non-negotiable: Space Grotesk + IBM Plex Mono)
mkdir -p "$APP/Contents/Resources/Fonts"
cp mac-shell/Resources/Fonts/*.ttf "$APP/Contents/Resources/Fonts/"
# Graphics fonts the CORE registers at runtime (process scope) for template
# graphics: Selawik for Segoe UI (owner decision 2026-11-21), shipped with its
# SIL OFL 1.1 text. They sit beside corevideo-native (GraphicsFonts/ next to the
# executable is the core's lookup), NOT in Fonts/, which ATSApplicationFontsPath
# registers for the shell's own UI. Resources only: sealed by the app signature
# below, no code to sign. Provenance: native/resources/graphics-fonts/SOURCE.md.
mkdir -p "$APP/Contents/Resources/GraphicsFonts"
cp native/resources/graphics-fonts/*.ttf native/resources/graphics-fonts/OFL.txt \
   "$APP/Contents/Resources/GraphicsFonts/"
# Built-in graphics style packs (packs/<id>/: templates, palettes, OFL fonts with
# OFL.txt, PNG assets). Data only, sealed by the app signature. The shell names
# this folder in COREVIDEO_GRAPHICS_PACKS_DIR for the pkg-installed core.
rm -rf "$APP/Contents/Resources/packs"
cp -R native/resources/graphics-packs "$APP/Contents/Resources/packs"
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
# Streaming (mac streaming Phase 2, F1 + P1): the core runs ffmpeg under
# corevideo-child-guard, both resolved beside the core in Resources. Without
# the guard a killed core leaves ffmpeg running; without ffmpeg nothing streams
# on a Mac that has no Homebrew.
if need_helper "$CORE_DIR/corevideo-child-guard" "ffmpeg child guard"; then
  cp "$CORE_DIR/corevideo-child-guard" "$APP/Contents/Resources/"
fi
FFMPEG_BUILT="${COREVIDEO_FFMPEG_BUILT_DIR:-}"
if need_helper "${FFMPEG_BUILT:-<COREVIDEO_FFMPEG_BUILT_DIR unset>}/bin/ffmpeg" "bundled LGPL ffmpeg (scripts/build-macos-ffmpeg.sh)"; then
  cp "$FFMPEG_BUILT/bin/ffmpeg" "$APP/Contents/Resources/ffmpeg"
  # Licence texts + provenance travel with the binary (LGPL notice obligation).
  mkdir -p "$APP/Contents/Resources/notices"
  cp -R "$FFMPEG_BUILT/notices/ffmpeg" "$APP/Contents/Resources/notices/"
fi
# corevideo-browser-host is deliberately NOT bundled: the shell never resolves
# it and never sets COREVIDEO_BROWSER_HOST_PATH (the core's only lookup on
# macOS), and the UI states browser sources are unavailable on macOS.
if need_helper "$ENGINE_SRC" "Zoom engine app"; then
  # ditto, not cp -R: keeps the SDK frameworks' Versions/Current symlinks.
  ditto "$ENGINE_SRC" "$APP/Contents/Resources/corevideo-zoom-engine.app"
fi
# The camera extension. Its Info.plist keys are checked one by one by
# scripts/check-macos-camera-extension.sh (b); keep the two in step.
EXT="$APP/Contents/Library/SystemExtensions/$EXT_ID.systemextension"
mkdir -p "$EXT/Contents/MacOS"
cp "$CAMERA_BIN" "$EXT/Contents/MacOS/CoreVideoProCamera"
# >>> camera-extension-hashed: scripts/camera-extension-version.sh hashes the
# lines between these markers; changing them needs an extensionVersion bump.
cat > "$EXT/Contents/Info.plist" <<PLIST
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>CFBundleExecutable</key><string>CoreVideoProCamera</string>
    <key>CFBundleIdentifier</key><string>$EXT_ID</string>
    <key>CFBundleName</key><string>CoreVideo Pro Camera</string>
    <key>CFBundlePackageType</key><string>SYSX</string>
    <key>CFBundleShortVersionString</key><string>$EXT_VERSION</string>
    <key>CFBundleVersion</key><string>$EXT_VERSION</string>
    <key>LSMinimumSystemVersion</key><string>13.0</string>
    <key>CMIOExtension</key>
    <dict>
        <key>CMIOExtensionMachServiceName</key><string>42394L3353.$EXT_ID</string>
    </dict>
    <key>NSSystemExtensionUsageDescription</key><string>Lets CoreVideo Pro offer its Program output as a camera in Zoom, Teams, FaceTime and other apps.</string>
</dict>
</plist>
PLIST
# <<< camera-extension-hashed
if [ "$SIGN_ID" = "-" ]; then
    # Extension first: signing the app seals it, and a nested signature made
    # afterwards would break that seal.
    codesign --force --sign - \
        --entitlements scripts/macos-camera-extension-adhoc.entitlements "$EXT"
    for helper in corevideo-child-guard ffmpeg; do
        if [ -f "$APP/Contents/Resources/$helper" ]; then
            codesign --force --sign - "$APP/Contents/Resources/$helper"
        fi
    done
    codesign --force --sign - --entitlements scripts/macos-shell.entitlements "$APP"
else
    # Profiles go in before any signature: each is sealed into its bundle.
    cp "$CAMERA_PROFILE" "$EXT/Contents/embedded.provisionprofile"
    cp "$APP_PROFILE" "$APP/Contents/embedded.provisionprofile"
    # The extension first (it is nested inside the app's seal), then the
    # helpers, then the app.
    sign_one "$EXT" --entitlements scripts/macos-camera-extension.entitlements
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
    # ffmpeg and its guard: hardened runtime, timestamp, NO entitlements (no
    # JIT, no unsigned memory, no foreign libraries: ffmpeg is one static
    # executable linking only the OS). Signed before the app seals them.
    if [ -f "$APP/Contents/Resources/corevideo-child-guard" ]; then
        sign_one "$APP/Contents/Resources/corevideo-child-guard" \
            --identifier us.iamfatness.corevideopro.child-guard
    fi
    if [ -f "$APP/Contents/Resources/ffmpeg" ]; then
        sign_one "$APP/Contents/Resources/ffmpeg" \
            --identifier us.iamfatness.corevideopro.ffmpeg
    fi
    sign_one "$APP" --entitlements scripts/macos-shell-sysext.entitlements
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
