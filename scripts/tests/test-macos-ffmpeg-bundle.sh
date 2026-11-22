#!/bin/bash
# The macOS app bundle carries the LGPL ffmpeg, its child guard and their
# notices (mac streaming Phase 2, F1 + P1). Assembles an AD-HOC bundle into a
# temp dir from the current builds (never the developer's dev bundle) and
# checks what a release would ship:
#   Contents/Resources/ffmpeg                 passes build-macos-ffmpeg.sh --audit
#   Contents/Resources/corevideo-child-guard  present, signature verifies
#   Contents/Resources/notices/ffmpeg/        licences + PROVENANCE.txt naming the lock's SHA-256s
#
#   scripts/tests/test-macos-ffmpeg-bundle.sh [core-build-dir]   (default native/build-metal)
# Needs: swift build -c release in mac-shell, the core build, and the ffmpeg
# build (run here; cached after the first time).
set -euo pipefail
cd "$(dirname "$0")/../.."
CORE_DIR="${1:-native/build-metal}"
fail=0
check() { if eval "$2"; then echo "ok   $1"; else echo "FAIL $1"; fail=1; fi; }

FFMPEG_DIR="$(scripts/build-macos-ffmpeg.sh | tail -1)"
OUT="$(mktemp -d "${TMPDIR:-/tmp}/cvp-bundle-test.XXXXXX")"
trap 'rm -rf "$OUT"' EXIT
APP="$OUT/CoreVideo Pro.app"
COREVIDEO_SIGN_IDENTITY=- COREVIDEO_CORE_BUILD_DIR="$CORE_DIR" COREVIDEO_FFMPEG_BUILT_DIR="$FFMPEG_DIR" \
    COREVIDEO_APP_OUT="$APP" scripts/make-macos-shell-bundle.sh >"$OUT/bundle.log" 2>&1 \
    || { cat "$OUT/bundle.log"; echo "FAIL bundle script"; exit 1; }
R="$APP/Contents/Resources"
check "ffmpeg is bundled" '[ -x "$R/ffmpeg" ]'
check "bundled ffmpeg passes the LGPL/capability audit" 'scripts/build-macos-ffmpeg.sh --audit "$R/ffmpeg" 2>/dev/null'
# Re-signing changes the bytes, so compare what the binary says it is.
check "bundled ffmpeg is the pinned build" '[ "$("$R/ffmpeg" -hide_banner -version | head -1)" = "$("$FFMPEG_DIR/bin/ffmpeg" -hide_banner -version | head -1)" ] && "$R/ffmpeg" -hide_banner -version | head -1 | grep -q "ffmpeg version $(sed -n "s/^FFMPEG_VERSION=//p" scripts/macos-ffmpeg.lock) "'
check "child guard is bundled" '[ -x "$R/corevideo-child-guard" ]'
check "ffmpeg signature verifies" 'codesign --verify --strict "$R/ffmpeg" 2>/dev/null'
check "child guard signature verifies" 'codesign --verify --strict "$R/corevideo-child-guard" 2>/dev/null'
check "app signature verifies (seals the helpers)" 'codesign --verify --deep --strict "$APP" 2>/dev/null'
check "LGPL v3 text ships" '[ -s "$R/notices/ffmpeg/FFMPEG-COPYING.LGPLv3" ]'
check "OpenSSL licence ships" '[ -s "$R/notices/ffmpeg/OPENSSL-LICENSE.txt" ]'
check "libsrt licence ships" '[ -s "$R/notices/ffmpeg/LIBSRT-LICENSE-MPL-2.0.txt" ]'
# shellcheck source=../macos-ffmpeg.lock
. scripts/macos-ffmpeg.lock
for sha in "$FFMPEG_SHA256" "$OPENSSL_SHA256" "$SRT_SHA256"; do
    check "PROVENANCE names $sha" 'grep -q "$sha" "$R/notices/ffmpeg/PROVENANCE.txt"'
done
check "PROVENANCE states LGPL v3 or later" 'grep -q "Licence: GNU LGPL version 3 or later" "$R/notices/ffmpeg/PROVENANCE.txt"'
[ "$fail" = 0 ] && echo "test-macos-ffmpeg-bundle: PASS" || { echo "test-macos-ffmpeg-bundle: FAIL"; exit 1; }
