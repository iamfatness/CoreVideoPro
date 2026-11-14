#!/bin/bash
# Build, sign, notarize and staple a CoreVideo Pro macOS release DMG.
#
#   scripts/release-macos.sh [--skip-notarize]
#
# Output: dist/macos/<version>/
#   CoreVideoPro-<version>-arm64.dmg         signed, notarized, stapled
#   CoreVideoPro-<version>-arm64.dmg.sha256  `shasum -a 256` line
#   manifest.json                            version, build, git sha, identity,
#                                            notarization ids, helper inventory
#
# Steps: clean Release build of the media core (CI's mac Metal config plus the
# adapters the shipped app uses: AVF capture, ScreenCaptureKit, RTMP output),
# the Zoom engine (real SDK) and the SwiftUI shell, each in a dedicated
# release build dir so dev trees are never touched; assemble + Developer ID
# sign the .app (make-macos-shell-bundle.sh); notarize the app (zip) and
# staple; build a DMG (app + /Applications alias, volume "CoreVideo Pro"),
# sign, notarize and staple it; verify; write dist/.
#
# Environment:
#   COREVIDEO_SIGN_IDENTITY   default "Developer ID Application: John Wallace (42394L3353)"
#   COREVIDEO_NOTARY_PROFILE  notarytool keychain profile, default "CoreVideo Plugin"
#   ZOOM_SDK_DIR              Zoom macOS SDK dir holding ZoomSDK.framework
#                             (default ~/Downloads/zoom-sdk-macos-7.1.5.84750/ZoomSDK)
#   COREVIDEO_BUILD_NUMBER    CFBundleVersion (default: git commit count)
#   COREVIDEO_ALLOW_DIRTY=1   build from a dirty tree (recorded in the manifest)
#
# --skip-notarize builds and signs everything but submits nothing to Apple
# (rehearsal); its output is NOT distributable and lands in dist/ marked so.
#
# Runtime note: RTMP streaming launches an external `ffmpeg` (found via
# COREVIDEO_FFMPEG_BIN_DIR, FFMPEG_BIN_DIR, the core's own directory or PATH).
# This DMG does not bundle ffmpeg (GPL/licensing ruling pending).
set -euo pipefail
cd "$(dirname "$0")/.."
ROOT="$(pwd)"

NOTARIZE=1
while [ $# -gt 0 ]; do
    case "$1" in
        --skip-notarize) NOTARIZE=0; shift ;;
        -h|--help) sed -n '2,33p' "$0"; exit 0 ;;
        *) echo "error: unknown argument '$1'" >&2; exit 2 ;;
    esac
done

SIGN_ID="${COREVIDEO_SIGN_IDENTITY:-Developer ID Application: John Wallace (42394L3353)}"
PROFILE="${COREVIDEO_NOTARY_PROFILE:-CoreVideo Plugin}"
ZOOM_SDK="${ZOOM_SDK_DIR:-$HOME/Downloads/zoom-sdk-macos-7.1.5.84750/ZoomSDK}"
export PATH="$HOME/Library/Python/3.9/bin:$PATH"   # pip-installed cmake/ninja

log() { printf '\n==> %s\n' "$*"; }
die() { echo "error: $*" >&2; exit 1; }

# ── Preflight: everything knowable before a 20-minute build ─────────────────
[ "$(uname -m)" = "arm64" ] || die "Apple Silicon only (policy); this host is $(uname -m)"
for tool in cmake ninja swift node xcrun hdiutil codesign ditto shasum; do
    command -v "$tool" >/dev/null || die "missing tool: $tool"
done
security find-identity -v -p codesigning | grep -qF "\"$SIGN_ID\"" \
    || die "signing identity not in keychain: $SIGN_ID"
[ -d "$ZOOM_SDK/ZoomSDK.framework" ] || die "no ZoomSDK.framework under ZOOM_SDK_DIR=$ZOOM_SDK"
if [ "$NOTARIZE" = "1" ]; then
    # Cheap auth probe: fails fast on a missing profile or bad credentials.
    xcrun notarytool history --keychain-profile "$PROFILE" >/dev/null 2>&1 \
        || die "notarytool profile '$PROFILE' missing or auth failed (xcrun notarytool history)"
fi
DIRTY=false
if [ -n "$(git status --porcelain --untracked-files=no)" ]; then
    DIRTY=true
    [ "${COREVIDEO_ALLOW_DIRTY:-0}" = "1" ] \
        || die "working tree has uncommitted changes (COREVIDEO_ALLOW_DIRTY=1 to override)"
fi
VERSION="$(node -p "require('./package.json').version")"
BUILD_NUMBER="${COREVIDEO_BUILD_NUMBER:-$(git rev-list --count HEAD)}"
GIT_SHA="$(git rev-parse HEAD)"
export COREVIDEO_BUILD_NUMBER="$BUILD_NUMBER"
echo "CoreVideo Pro $VERSION (build $BUILD_NUMBER, $GIT_SHA, dirty=$DIRTY)"

CORE_BUILD="native/build-release-core"
ENGINE_BUILD="native/build-release-engine"
SHELL_SCRATCH="mac-shell/.build-release"
APP="mac-shell/.build/CoreVideo Pro.app"
OUT="dist/macos/$VERSION"
DMG_NAME="CoreVideoPro-$VERSION-arm64.dmg"
WORK="$(mktemp -d "${TMPDIR:-/tmp}/cvp-release.XXXXXX")"
MOUNT=""
cleanup() {
    [ -z "$MOUNT" ] || hdiutil detach "$MOUNT" -quiet -force 2>/dev/null || true
    rm -rf "$WORK"
}
trap cleanup EXIT

# ── Clean builds ────────────────────────────────────────────────────────────
log "core (clean Release: Metal + AVF encoder/capture + SCK + CoreAudio + RTMP)"
rm -rf "$CORE_BUILD"
cmake -S native -B "$CORE_BUILD" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DCOREVIDEO_STUB=OFF -DCOREVIDEO_ENABLE_DEV_ADAPTERS=ON \
    -DCOREVIDEO_WITH_METAL=ON \
    -DCOREVIDEO_WITH_AVF_ENCODER=ON \
    -DCOREVIDEO_WITH_COREAUDIO=ON \
    -DCOREVIDEO_WITH_AVF_CAPTURE=ON -DCOREVIDEO_WITH_SCK=ON \
    -DCOREVIDEO_WITH_RTMP_OUTPUT=ON
cmake --build "$CORE_BUILD" --target corevideo-native corevideo-plugin-host

log "Zoom engine (clean Release, SDK $ZOOM_SDK)"
rm -rf "$ENGINE_BUILD"
cmake -S native -B "$ENGINE_BUILD" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DCOREVIDEO_ENABLE_DEV_ADAPTERS=ON -DCOREVIDEO_STUB=OFF \
    -DCOREVIDEO_BUILD_ZOOM_ENGINE=ON -DZOOM_SDK_DIR="$ZOOM_SDK"
cmake --build "$ENGINE_BUILD" --target corevideo-zoom-engine
# Ad-hoc here on purpose: make-macos-shell-bundle.sh re-signs the whole engine
# (SDK deepest-first) with the Developer ID once it sits inside the app, so a
# Developer ID pass now would only double the ~75 timestamp round-trips.
scripts/make-macos-engine-bundle.sh --build-dir "$ENGINE_BUILD" \
    --zoom-sdk "$ZOOM_SDK" --sign -

log "shell (clean swift build -c release)"
rm -rf "$SHELL_SCRATCH"
swift build -c release --package-path mac-shell --scratch-path "$SHELL_SCRATCH"
SHELL_BIN="$SHELL_SCRATCH/release/CoreVideoProShell"

# ── Bundle + sign ───────────────────────────────────────────────────────────
log "assemble + sign $APP"
COREVIDEO_SIGN_IDENTITY="$SIGN_ID" \
COREVIDEO_SHELL_BIN="$SHELL_BIN" \
COREVIDEO_CORE_BUILD_DIR="$CORE_BUILD" \
COREVIDEO_ENGINE_APP="$ENGINE_BUILD/corevideo-zoom-engine.app" \
COREVIDEO_REQUIRE_HELPERS=1 \
    scripts/make-macos-shell-bundle.sh

# Every Mach-O in the app must be arm64-capable and carry a Developer ID
# signature with the hardened runtime. Catch it here, not in Apple's log.
log "audit every Mach-O in the app"
MACHO_COUNT=0
while IFS= read -r f; do
    magic="$(head -c 4 "$f" | od -An -tx1 | tr -d ' \n')"
    case "$magic" in feedfacf|cffaedfe|cafebabe|bebafeca|feedface|cefaedfe) ;; *) continue ;; esac
    MACHO_COUNT=$((MACHO_COUNT + 1))
    lipo -archs "$f" 2>/dev/null | grep -qw arm64 || die "not arm64: $f"
    info="$(codesign -dvv "$f" 2>&1)" || die "unsigned: $f"
    grep -qF "Authority=$SIGN_ID" <<<"$info" || die "not signed by $SIGN_ID: $f"
    grep -q "flags=.*runtime" <<<"$info" || die "no hardened runtime: $f"
    grep -q "^Timestamp=" <<<"$info" || die "no secure timestamp: $f"
done < <(find "$APP" -type f)
echo "audited $MACHO_COUNT Mach-O files: arm64 + Developer ID + runtime + timestamp"

# notarytool submit --wait, JSON out; echoes the submission id. Rejected or
# invalid → fetch the log into $OUT and fail.
NOTARY_LOG_DIR="$WORK"
notarize() {  # notarize <file> <label>
    local file="$1" label="$2" json id status
    echo "submitting $label to Apple notary ($(du -h "$file" | cut -f1))..." >&2
    json="$(xcrun notarytool submit "$file" --keychain-profile "$PROFILE" \
            --wait --timeout 2h --output-format json)" || {
        echo "$json" >&2; die "notarytool submit failed for $label"; }
    id="$(python3 -I -c 'import json,sys; print(json.load(sys.stdin)["id"])' <<<"$json")"
    status="$(python3 -I -c 'import json,sys; print(json.load(sys.stdin)["status"])' <<<"$json")"
    echo "$label: submission $id -> $status" >&2
    xcrun notarytool log "$id" --keychain-profile "$PROFILE" \
        "$NOTARY_LOG_DIR/notary-$label.json" >/dev/null 2>&1 || true
    [ "$status" = "Accepted" ] || {
        cat "$NOTARY_LOG_DIR/notary-$label.json" >&2 2>/dev/null || true
        die "$label notarization $status (id $id)"; }
    echo "$id"
}

APP_SUBMISSION="skipped"
DMG_SUBMISSION="skipped"
if [ "$NOTARIZE" = "1" ]; then
    log "notarize app"
    # ditto --keepParent, never plain zip: keeps framework symlinks intact.
    ditto -c -k --keepParent "$APP" "$WORK/CoreVideoPro.zip"
    APP_SUBMISSION="$(notarize "$WORK/CoreVideoPro.zip" app)"
    xcrun stapler staple "$APP"
    xcrun stapler validate "$APP"
fi

# ── DMG ─────────────────────────────────────────────────────────────────────
log "build DMG"
STAGE="$WORK/dmg"
mkdir -p "$STAGE"
ditto "$APP" "$STAGE/CoreVideo Pro.app"
ln -s /Applications "$STAGE/Applications"
rm -rf "$OUT"
mkdir -p "$OUT"
DMG="$OUT/$DMG_NAME"
hdiutil create -volname "CoreVideo Pro" -srcfolder "$STAGE" -fs HFS+ \
    -format UDZO -imagekey zlib-level=9 -ov "$DMG" >/dev/null
# A DMG takes a plain signature: no hardened runtime (not code), but a
# secure timestamp so it outlives the certificate.
codesign --force --sign "$SIGN_ID" --timestamp \
    --identifier us.iamfatness.corevideopro.dmg "$DMG"
codesign --verify --strict "$DMG"

if [ "$NOTARIZE" = "1" ]; then
    log "notarize DMG"
    DMG_SUBMISSION="$(notarize "$DMG" dmg)"
    xcrun stapler staple "$DMG"
    xcrun stapler validate "$DMG"
    cp "$WORK"/notary-*.json "$OUT/" 2>/dev/null || true
fi

# ── Verify (the questions a user's Mac asks) ────────────────────────────────
log "verify"
codesign --verify --deep --strict --verbose=2 "$APP"
if [ "$NOTARIZE" = "1" ]; then
    spctl --assess --type execute -vv "$APP" 2>&1 | tee "$WORK/spctl-app.txt"
    grep -q "source=Notarized Developer ID" "$WORK/spctl-app.txt" \
        || die "app is not assessed as Notarized Developer ID"
    spctl --assess --type open --context context:primary-signature -vv "$DMG" 2>&1 \
        | tee "$WORK/spctl-dmg.txt"
    grep -q "source=Notarized Developer ID" "$WORK/spctl-dmg.txt" \
        || die "DMG is not assessed as Notarized Developer ID"
    # The copy a user actually drags out: mount and assess the app inside.
    MOUNT="$WORK/mnt"; mkdir -p "$MOUNT"
    hdiutil attach "$DMG" -readonly -nobrowse -noautoopen -mountpoint "$MOUNT" -quiet
    [ -L "$MOUNT/Applications" ] || die "DMG lacks the Applications alias"
    xcrun stapler validate "$MOUNT/CoreVideo Pro.app"
    spctl --assess --type execute -vv "$MOUNT/CoreVideo Pro.app" 2>&1 \
        | grep -q "source=Notarized Developer ID" || die "app inside DMG not notarized"
    hdiutil detach "$MOUNT" -quiet; MOUNT=""
    echo "app inside the mounted DMG: stapled + Notarized Developer ID"
fi

# ── Artifacts ───────────────────────────────────────────────────────────────
log "write $OUT"
( cd "$OUT" && shasum -a 256 "$DMG_NAME" > "$DMG_NAME.sha256" )
SHA256="$(cut -d' ' -f1 "$OUT/$DMG_NAME.sha256")"
HELPERS="$(cd "$APP/Contents/Resources" && ls -d corevideo-* | paste -sd, -)"
python3 -I - "$OUT/manifest.json" <<PY
import json, sys
manifest = {
    "product": "CoreVideo Pro",
    "platform": "macOS",
    "arch": "arm64",
    "version": "$VERSION",
    "build": int("$BUILD_NUMBER"),
    "bundleId": "us.iamfatness.corevideopro.shell",
    "gitSha": "$GIT_SHA",
    "gitDirty": $( [ "$DIRTY" = true ] && echo True || echo False ),
    "signingIdentity": "$SIGN_ID",
    "notarized": $( [ "$NOTARIZE" = 1 ] && echo True || echo False ),
    "notarization": {
        "keychainProfile": "$PROFILE",
        "appSubmissionId": "$APP_SUBMISSION",
        "dmgSubmissionId": "$DMG_SUBMISSION",
        "stapled": $( [ "$NOTARIZE" = 1 ] && echo True || echo False ),
    },
    "dmg": {"file": "$DMG_NAME", "sha256": "$SHA256", "volumeName": "CoreVideo Pro"},
    "bundledHelpers": "$HELPERS".split(","),
    "zoomSdk": "$(basename "$(dirname "$ZOOM_SDK")")",
    "runtimeRequirements": [
        "macOS 13+ on Apple Silicon",
        "RTMP streaming needs an ffmpeg executable (not bundled)",
    ],
}
with open(sys.argv[1], "w") as f:
    json.dump(manifest, f, indent=2)
    f.write("\n")
PY
cat "$OUT/manifest.json"
[ "$NOTARIZE" = "1" ] || echo "NOTE: --skip-notarize build; NOT distributable"
echo "release: $DMG"
