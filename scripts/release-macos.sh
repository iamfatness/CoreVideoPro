#!/bin/bash
# Build, sign, notarize and staple a CoreVideo Pro macOS release DMG.
#
#   scripts/release-macos.sh [--skip-notarize] [--skip-dmg]
#
# Output: dist/macos/<version>/
#   CoreVideoPro-<version>-arm64.dmg         signed, notarized, stapled
#   CoreVideoPro-<version>-arm64.dmg.sha256  `shasum -a 256` line
#   CoreVideoPro-<version>-arm64.pkg         guided installer (/Applications),
#                                            signed Developer ID Installer,
#                                            notarized, stapled
#   CoreVideoPro-<version>-arm64.pkg.sha256
#   manifest.json                            version, build, git sha, identity,
#                                            notarization ids, helper inventory
#
# Steps: clean Release build of the media core (CI's mac Metal config plus the
# adapters the shipped app uses: AVF capture, ScreenCaptureKit, RTMP output),
# then the FULL native ctest on that exact build -- including the `realtime`
# label (AVF encoder/capture timing tests) that shared CI runners exclude, so
# this script is the gate that runs them; any failure stops the release. Then
# the Zoom engine (real SDK) and the SwiftUI shell, each in a dedicated
# release build dir so dev trees are never touched; assemble + Developer ID
# sign the .app (make-macos-shell-bundle.sh) inside the temp work dir, so the
# dev bundle at mac-shell/.build/CoreVideo Pro.app is never touched; notarize the app (zip) and
# staple; build a DMG (app + /Applications alias, volume "CoreVideo Pro"),
# sign, notarize and staple it; verify; write dist/.
#
# Environment:
#   COREVIDEO_SIGN_IDENTITY   default "Developer ID Application: John Wallace (42394L3353)"
#   COREVIDEO_PKG_SIGN_IDENTITY  default "Developer ID Installer: John Wallace (42394L3353)"
#                             (the .pkg needs this, NOT the Application certificate)
#   COREVIDEO_NOTARY_PROFILE  notarytool keychain profile, default "CoreVideo Pro"
#   COREVIDEO_APP_PROFILE     Developer ID provisioning profile for the app
#   COREVIDEO_CAMERA_PROFILE  ... for the camera extension (defaults under
#                             ~/Library/Developer/CoreVideoPro/profiles/). Both
#                             are verified BEFORE the build; the release FAILS if
#                             the signing certificate expires within 14 days and
#                             warns within 60.
#                             (one-time: xcrun notarytool store-credentials "CoreVideo Pro"
#                             --team-id 42394L3353; "CoreVideo Plugin" is the OBS plugin's)
#   ZOOM_SDK_DIR              Zoom macOS SDK dir holding ZoomSDK.framework
#                             (default ~/Downloads/zoom-sdk-macos-7.1.5.84750/ZoomSDK)
#   COREVIDEO_BUILD_NUMBER    CFBundleVersion (default: git commit count)
#   COREVIDEO_ALLOW_DIRTY=1   build from a dirty tree (recorded in the manifest)
#
# --skip-notarize builds and signs everything but submits nothing to Apple
# (rehearsal). Its output can never be mistaken for, or overwrite, a release:
#   dist/macos/<version>-UNNOTARIZED/CoreVideoPro-<version>-arm64-UNNOTARIZED.dmg
#
# Everything is built in a temp dir and moved into dist/ only after every
# verification passed, so a failed run never leaves a plausible DMG behind
# (and never removes the previous good output).
#
# Runtime note: RTMP output launches an external `ffmpeg`, which this DMG does
# not bundle (GPL/licensing ruling pending). The release core is built with
# COREVIDEO_WITH_RTMP_OUTPUT=ON only; SRT output and SRT/RTMP ingest are not in
# the macOS 1.0 build (their options default OFF and the Mac shell has no SRT UI).
# The core looks in COREVIDEO_FFMPEG_BIN_DIR, FFMPEG_BIN_DIR, its own
# directory, the cwd, PATH, then /opt/homebrew/bin, /usr/local/bin and
# /opt/local/bin (native/src/modules/PosixExecutableLocator.h), so a
# Homebrew or MacPorts ffmpeg is found even from a Finder/Dock launch, whose
# PATH is only /usr/bin:/bin:/usr/sbin:/sbin.
set -euo pipefail
cd "$(dirname "$0")/.."
ROOT="$(pwd)"

NOTARIZE=1
MAKE_DMG=1
while [ $# -gt 0 ]; do
    case "$1" in
        --skip-notarize) NOTARIZE=0; shift ;;
        # --skip-dmg: build only the .pkg. Needed when macOS App Management
        # blocks the host terminal from writing an .app copy into hdiutil's
        # temporary volume ("Could not access /Volumes/CoreVideo Pro/...app
        # - Operation not permitted"), which starts once a signed CoreVideo
        # Pro is installed on the build Mac. The fix is granting the terminal
        # App Management; this flag ships the pkg in the meantime.
        --skip-dmg) MAKE_DMG=0; shift ;;
        -h|--help) sed -n '2,50p' "$0"; exit 0 ;;
        *) echo "error: unknown argument '$1'" >&2; exit 2 ;;
    esac
done

SIGN_ID="${COREVIDEO_SIGN_IDENTITY:-Developer ID Application: John Wallace (42394L3353)}"
PROFILE="${COREVIDEO_NOTARY_PROFILE:-CoreVideo Pro}"
# The .pkg is signed with the Developer ID INSTALLER certificate, a different
# certificate from the Application one above.
PKG_SIGN_ID="${COREVIDEO_PKG_SIGN_IDENTITY:-Developer ID Installer: John Wallace (42394L3353)}"
ZOOM_SDK="${ZOOM_SDK_DIR:-$HOME/Downloads/zoom-sdk-macos-7.1.5.84750/ZoomSDK}"
export PATH="$HOME/Library/Python/3.9/bin:$PATH"   # pip-installed cmake/ninja

log() { printf '\n==> %s\n' "$*"; }
die() { echo "error: $*" >&2; exit 1; }

# ── Preflight: everything knowable before a 20-minute build ─────────────────
[ "$(uname -m)" = "arm64" ] || die "Apple Silicon only (policy); this host is $(uname -m)"
for tool in cmake ninja swift node xcrun hdiutil codesign ditto shasum; do
    command -v "$tool" >/dev/null || die "missing tool: $tool"
done
# Never `cmd | grep -q` under pipefail: grep -q exits on the first match and
# the producer's SIGPIPE turns a hit into a failure. Capture, then match.
IDENTITIES="$(security find-identity -v -p codesigning)"
grep -qF "\"$SIGN_ID\"" <<<"$IDENTITIES" || die "signing identity not in keychain: $SIGN_ID"
case "$PKG_SIGN_ID" in *"Developer ID Installer"*) ;; *) die "COREVIDEO_PKG_SIGN_IDENTITY must be a \"Developer ID Installer\" identity, got: $PKG_SIGN_ID" ;; esac
# -p basic: installer certs are not code-signing identities, so the
# `-p codesigning` list above never shows them.
PKG_IDENTITIES="$(security find-identity -v)"
grep -qF "\"$PKG_SIGN_ID\"" <<<"$PKG_IDENTITIES" \
    || die "installer signing identity not in keychain: $PKG_SIGN_ID (the .pkg needs the Developer ID Installer certificate, not the Application one)"
for tool in pkgbuild productbuild pkgutil spctl; do
    command -v "$tool" >/dev/null || die "missing tool: $tool"
done
# Provisioning profiles + certificate lifetime, before a 20-minute build (a
# mismatched profile does not fail codesign; it fails activation silently).
# verify_profile checks the profile's app id, team, expiry, the signing
# certificate's expiry and that certificate's membership in the profile.
. scripts/macos-sign-lib.sh
APP_PROFILE="${COREVIDEO_APP_PROFILE:-$HOME/Library/Developer/CoreVideoPro/profiles/CoreVideo_Pro_macOS_Developer_ID.provisionprofile}"
CAMERA_PROFILE="${COREVIDEO_CAMERA_PROFILE:-$HOME/Library/Developer/CoreVideoPro/profiles/CoreVideo_Pro_Camera_Extension_Developer_ID.provisionprofile}"
for pair in "COREVIDEO_APP_PROFILE|$APP_PROFILE|42394L3353.us.iamfatness.corevideopro.shell" \
            "COREVIDEO_CAMERA_PROFILE|$CAMERA_PROFILE|42394L3353.us.iamfatness.corevideopro.shell.camera"; do
    IFS='|' read -r pvar ppath pid <<<"$pair"
    [ -f "$ppath" ] || die "$pvar: provisioning profile not found at $ppath"
    verify_profile "$ppath" "$pid" 14 \
        || die "$pvar: $ppath failed verification (see above; the release fails with under 14 days of profile or signing-certificate life)"
    verify_profile "$ppath" "$pid" 60 >/dev/null 2>&1 \
        || echo "WARNING: $pvar: profile or signing certificate expires within 60 days; renew before the next release" >&2
done
[ -d "$ZOOM_SDK/ZoomSDK.framework" ] || die "no ZoomSDK.framework under ZOOM_SDK_DIR=$ZOOM_SDK"
if [ "$NOTARIZE" = "1" ]; then
    # Cheap auth probe: fails fast on a missing profile or bad credentials.
    xcrun notarytool history --keychain-profile "$PROFILE" >/dev/null 2>&1 \
        || die "notarytool profile '$PROFILE' missing or auth failed (xcrun notarytool history); create it once with: xcrun notarytool store-credentials \"$PROFILE\" --team-id 42394L3353"
fi
# The camera extension's own version must move with its sources: an
# unbumped change would ship a different extension under the installed
# version (never replaced), a needless bump forces every user through a swap.
scripts/camera-extension-version.sh check || die "camera extension version gate failed (see above)"
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
if [ "$NOTARIZE" = "1" ]; then
    OUT="dist/macos/$VERSION"
    DMG_NAME="CoreVideoPro-$VERSION-arm64.dmg"
    PKG_NAME="CoreVideoPro-$VERSION-arm64.pkg"
else
    OUT="dist/macos/$VERSION-UNNOTARIZED"
    DMG_NAME="CoreVideoPro-$VERSION-arm64-UNNOTARIZED.dmg"
    PKG_NAME="CoreVideoPro-$VERSION-arm64-UNNOTARIZED.pkg"
fi
WORK="$(mktemp -d "${TMPDIR:-/tmp}/cvp-release.XXXXXX")"
# Assembled in the work dir, never at mac-shell/.build/CoreVideo Pro.app (the
# developer's dev bundle, which a release must leave untouched).
APP="$WORK/app/CoreVideo Pro.app"
MOUNT=""
cleanup() {
    [ -z "$MOUNT" ] || hdiutil detach "$MOUNT" -quiet -force 2>/dev/null || true
    rm -rf "$WORK"
    # A leftover moved-aside previous release is removed only once the new one is
    # in place; if the publish failed and the restore did too, it is the only copy.
    [ -z "${OUT:-}" ] || [ ! -e "$OUT" ] || rm -rf "$OUT.previous.$$"
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
    -DCOREVIDEO_WITH_RTMP_OUTPUT=ON \
    -DCOREVIDEO_WITH_MAC_VIRTUALCAM=ON
cmake --build "$CORE_BUILD" --target corevideo-native corevideo-plugin-host

# The release gate for the core: the FULL native suite on this exact build,
# INCLUDING the `realtime` label. CI excludes `realtime` (shared runners cannot
# hold AVF encoder/capture timing), so nothing else runs those tests; a release
# that skipped them would ship that path unverified. Run on a quiet machine.
log "core tests (full ctest, realtime label included)"
cmake --build "$CORE_BUILD"   # every test target ctest lists
CTEST_LOG="$WORK/ctest-core.log"
if ! ctest --test-dir "$CORE_BUILD" -j 6 --output-on-failure 2>&1 | tee "$CTEST_LOG"; then
    die "native ctest failed on the release core build (see output above); release stopped"
fi
# ctest prints "100% tests passed out of N" (all green) or "NN% tests passed,
# M tests failed out of N"; both contain "% tests passed".
CTEST_SUMMARY="$(grep -E '% tests passed' "$CTEST_LOG" | tail -1 || true)"
[ -n "$CTEST_SUMMARY" ] || die "could not read the ctest summary from $CTEST_LOG"
REALTIME_SUMMARY="$(ctest --test-dir "$CORE_BUILD" -N -L realtime | grep -E '^Total Tests' || true)"
# A gate that silently ran zero realtime tests is no gate.
case "$REALTIME_SUMMARY" in
    "Total Tests: 0"|"") die "the release core lists no 'realtime' ctest; the AVF timing gate did not run" ;;
esac
echo "core ctest: $CTEST_SUMMARY (realtime label: ${REALTIME_SUMMARY:-none listed})"

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
COREVIDEO_CAMERA_EXT_BIN="$SHELL_SCRATCH/release/CoreVideoProCamera" \
COREVIDEO_CORE_BUILD_DIR="$CORE_BUILD" \
COREVIDEO_ENGINE_APP="$ENGINE_BUILD/corevideo-zoom-engine.app" \
COREVIDEO_REQUIRE_HELPERS=1 \
COREVIDEO_APP_OUT="$APP" \
    scripts/make-macos-shell-bundle.sh
[ -d "$APP/Contents/MacOS" ] || die "bundle was not assembled at $APP"

# Every Mach-O in the app must be arm64-capable and carry a Developer ID
# signature with the hardened runtime. Catch it here, not in Apple's log.
log "audit every Mach-O in the app"
MACHO_COUNT=0
X86_VENDOR=0
while IFS= read -r f; do
    magic="$(head -c 4 "$f" | od -An -tx1 | tr -d ' \n')"
    case "$magic" in feedfacf|cffaedfe|cafebabe|bebafeca|feedface|cefaedfe) ;; *) continue ;; esac
    MACHO_COUNT=$((MACHO_COUNT + 1))
    archs="$(lipo -archs "$f" 2>/dev/null || true)"
    if ! grep -qw arm64 <<<"$archs"; then
        # Zoom ships a few Intel-only libraries (e.g. libmkldnn, MKL-DNN) that
        # it loads only on x86_64. Vendor SDK files are not ours to strip;
        # anything we build must run on Apple Silicon.
        case "$f" in
            */corevideo-zoom-engine.app/Contents/Frameworks/*)
                X86_VENDOR=$((X86_VENDOR + 1))
                echo "note: vendor SDK file has no arm64 slice (signed anyway): ${f#"$APP"/}" ;;
            *) die "not arm64: $f" ;;
        esac
    fi
    info="$(codesign -dvv "$f" 2>&1)" || die "unsigned: $f"
    grep -qF "Authority=$SIGN_ID" <<<"$info" || die "not signed by $SIGN_ID: $f"
    grep -q "flags=.*runtime" <<<"$info" || die "no hardened runtime: $f"
    grep -q "^Timestamp=" <<<"$info" || die "no secure timestamp: $f"
done < <(find "$APP" -type f)
# The find above runs in a process substitution, whose failure set -e cannot see:
# an audit that looked at nothing must not pass.
[ "$MACHO_COUNT" -gt 0 ] || die "audited 0 Mach-O files under $APP; nothing was checked"
echo "audited $MACHO_COUNT Mach-O files: Developer ID + runtime + timestamp; arm64 except $X86_VENDOR Intel-only vendor SDK file(s)"

# The camera extension's structure, signature, profiles and entitlements.
log "check the camera extension"
scripts/check-macos-camera-extension.sh --release "$APP" || die "camera extension check failed"
EXT_DIR="$APP/Contents/Library/SystemExtensions/us.iamfatness.corevideopro.shell.camera.systemextension"
EXT_BUNDLE_VERSION="$(/usr/libexec/PlistBuddy -c 'Print :CFBundleVersion' "$EXT_DIR/Contents/Info.plist")"
[ "$EXT_BUNDLE_VERSION" = "$(scripts/camera-extension-version.sh version)" ] \
    || die "camera extension CFBundleVersion $EXT_BUNDLE_VERSION is not CameraConstants.extensionVersion"
CAM_PROFILE_PLIST="$WORK/camera-profile.plist"
security cms -D -i "$CAMERA_PROFILE" >"$CAM_PROFILE_PLIST" 2>/dev/null || die "cannot decode $CAMERA_PROFILE"
CAM_PROFILE_EXPIRES="$(plutil -extract ExpirationDate raw -o - "$CAM_PROFILE_PLIST")"
( cd "$WORK" && codesign -d --extract-certificates=leaf "$EXT_DIR" >/dev/null 2>&1 ) \
    && openssl x509 -inform DER -in "$WORK/leaf0" -noout -enddate >"$WORK/cert-end.txt" 2>/dev/null \
    || die "cannot read the camera extension's signing certificate"
CERT_END="$(sed 's/^notAfter=//' "$WORK/cert-end.txt")"
CERT_EXPIRES="$(date -j -u -f '%b %e %T %Y %Z' "$CERT_END" '+%Y-%m-%dT%H:%M:%SZ' 2>/dev/null)" \
    || die "cannot parse the signing certificate expiry '$CERT_END'"
echo "camera extension: version $EXT_BUNDLE_VERSION, profile expires $CAM_PROFILE_EXPIRES, certificate expires $CERT_EXPIRES"

# notarytool submit --wait, JSON out; echoes the submission id. Rejected or
# invalid → fetch the log into $WORK (copied to dist/ on success) and fail.
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
SHA256=""
if [ "$NOTARIZE" = "1" ]; then
    log "notarize app"
    # ditto --keepParent, never plain zip: keeps framework symlinks intact.
    ditto -c -k --keepParent "$APP" "$WORK/CoreVideoPro.zip"
    APP_SUBMISSION="$(notarize "$WORK/CoreVideoPro.zip" app)"
    xcrun stapler staple "$APP"
    xcrun stapler validate "$APP"
fi

# ── DMG ─────────────────────────────────────────────────────────────────────
# Assembled in $WORK; published to $OUT only after verification.
PUB="$WORK/out"
mkdir -p "$PUB"
if [ "$MAKE_DMG" = "1" ]; then
log "build DMG"
STAGE="$WORK/dmg"
mkdir -p "$STAGE"
ditto "$APP" "$STAGE/CoreVideo Pro.app"
ln -s /Applications "$STAGE/Applications"
DMG="$PUB/$DMG_NAME"
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
    cp "$WORK"/notary-*.json "$PUB/" 2>/dev/null || true
fi
fi

# ── PKG (guided installer; built from the stapled app) ──────────────────────
log "build + sign .pkg"
PKG="$PUB/$PKG_NAME"
scripts/make-macos-pkg.sh --app "$APP" --version "$VERSION" --out "$PKG" --sign "$PKG_SIGN_ID"
PKG_SUBMISSION="skipped"
if [ "$NOTARIZE" = "1" ]; then
    log "notarize .pkg"
    PKG_SUBMISSION="$(notarize "$PKG" pkg)"
    xcrun stapler staple "$PKG"
    xcrun stapler validate "$PKG"
    cp "$WORK"/notary-pkg.json "$PUB/" 2>/dev/null || true
fi

# ── Verify (the questions a user's Mac asks) ────────────────────────────────
log "verify"
codesign --verify --deep --strict --verbose=2 "$APP"
if [ "$NOTARIZE" = "1" ]; then
    assess="$(spctl --assess --type execute -vv "$APP" 2>&1 || true)"
    echo "$assess"
    grep -q "source=Notarized Developer ID" <<<"$assess" \
        || die "app is not assessed as Notarized Developer ID"
  if [ "$MAKE_DMG" = "1" ]; then
    assess="$(spctl --assess --type open --context context:primary-signature -vv "$DMG" 2>&1 || true)"
    echo "$assess"
    grep -q "source=Notarized Developer ID" <<<"$assess" \
        || die "DMG is not assessed as Notarized Developer ID"
    # The copy a user actually drags out: mount and assess the app inside.
    MOUNT="$WORK/mnt"; mkdir -p "$MOUNT"
    hdiutil attach "$DMG" -readonly -nobrowse -noautoopen -mountpoint "$MOUNT" -quiet
    [ -L "$MOUNT/Applications" ] || die "DMG lacks the Applications alias"
    xcrun stapler validate "$MOUNT/CoreVideo Pro.app"
    assess="$(spctl --assess --type execute -vv "$MOUNT/CoreVideo Pro.app" 2>&1 || true)"
    grep -q "source=Notarized Developer ID" <<<"$assess" || die "app inside DMG not notarized"
    hdiutil detach "$MOUNT" -quiet; MOUNT=""
    echo "app inside the mounted DMG: stapled + Notarized Developer ID"
  fi
fi

# Capture first: `cmd | grep -q` under pipefail can fail on SIGPIPE.
pkgsig="$(pkgutil --check-signature "$PKG" 2>&1 || true)"
grep -q "Developer ID Installer" <<<"$pkgsig" \
    || die "pkg is not signed by a Developer ID Installer identity"
if [ "$NOTARIZE" = "1" ]; then
    assess="$(spctl -a -vv -t install "$PKG" 2>&1 || true)"
    echo "$assess"
    grep -q "source=Notarized Developer ID" <<<"$assess" \
        || die "pkg is not assessed as Notarized Developer ID"
fi

# ── Artifacts ───────────────────────────────────────────────────────────────
log "write $OUT"
if [ "$MAKE_DMG" = "1" ]; then
    ( cd "$PUB" && shasum -a 256 "$DMG_NAME" > "$DMG_NAME.sha256" )
    SHA256="$(cut -d' ' -f1 "$PUB/$DMG_NAME.sha256")"
fi
( cd "$PUB" && shasum -a 256 "$PKG_NAME" > "$PKG_NAME.sha256" )
PKG_SHA256="$(cut -d' ' -f1 "$PUB/$PKG_NAME.sha256")"
HELPERS="$(cd "$APP/Contents/Resources" && ls -d corevideo-* | paste -sd, -)"
python3 -I - "$PUB/manifest.json" <<PY
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
        "pkgSubmissionId": "$PKG_SUBMISSION",
        "stapled": $( [ "$NOTARIZE" = 1 ] && echo True || echo False ),
    },
    "dmg": ({"file": "$DMG_NAME", "sha256": "$SHA256", "volumeName": "CoreVideo Pro"}
            if "$MAKE_DMG" == "1" else None),
    "pkg": {"file": "$PKG_NAME", "sha256": "$PKG_SHA256",
            "identifier": "us.iamfatness.corevideopro.pkg",
            "signingIdentity": "$PKG_SIGN_ID",
            "submissionId": "$PKG_SUBMISSION",
            "notarized": $( [ "$NOTARIZE" = 1 ] && echo True || echo False ),
            "stapled": $( [ "$NOTARIZE" = 1 ] && echo True || echo False )},
    "cameraExtension": {"bundleVersion": "$EXT_BUNDLE_VERSION",
                        "profileExpires": "$CAM_PROFILE_EXPIRES",
                        "certificateExpires": "$CERT_EXPIRES"},
    "bundledHelpers": "$HELPERS".split(","),
    "zoomSdk": "$(basename "$(dirname "$ZOOM_SDK")")",
    "runtimeRequirements": [
        "macOS 13+ on Apple Silicon",
        "RTMP output needs ffmpeg (not bundled; e.g. brew install ffmpeg). "
        "Homebrew and MacPorts locations are found automatically.",
    ],
    "coreTests": {"ctest": "$CTEST_SUMMARY", "realtimeIncluded": True,
                  "realtime": "$REALTIME_SUMMARY"},
}
with open(sys.argv[1], "w") as f:
    json.dump(manifest, f, indent=2)
    f.write("\n")
PY
# Publish: only now does anything replace the previous output in dist/.
mkdir -p "$(dirname "$OUT")"
# Never delete the previous release before the new one is in place: move it
# aside, publish, then discard it. A failed mv restores the old directory.
OLD="$OUT.previous.$$"
[ -e "$OUT" ] && mv "$OUT" "$OLD"
if ! mv "$PUB" "$OUT"; then
    [ -e "$OLD" ] && mv "$OLD" "$OUT"
    echo "ERROR: could not publish $PUB to $OUT" >&2
    exit 1
fi
rm -rf "$OLD"
cat "$OUT/manifest.json"
[ "$NOTARIZE" = "1" ] || echo "NOTE: --skip-notarize build; NOT distributable"
[ "$MAKE_DMG" = "1" ] && echo "release: $OUT/$DMG_NAME" || echo "NOTE: --skip-dmg build; no DMG"
echo "release: $OUT/$PKG_NAME"
