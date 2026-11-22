#!/bin/bash
# Build the LGPL ffmpeg that the macOS release bundles beside corevideo-native.
#
#   scripts/build-macos-ffmpeg.sh            build (or reuse the cache); print the output dir
#   scripts/build-macos-ffmpeg.sh --audit <path/to/ffmpeg>
#   scripts/build-macos-ffmpeg.sh --source-bundle <dir>
#       copy the corresponding source (the three pinned tarballs, SHA-256
#       checked, + this script + the lock) into <dir>: the assets each Mac
#       GitHub release publishes (owner decision 2026-11-21), which the
#       app's notices/ffmpeg/SOURCE.txt points at.
#                                            licence + capability audit only (exit 1 on any gap)
#
# Output dir (printed as the LAST line of stdout):
#   <cache>/<key>/bin/ffmpeg          one arm64 executable; OpenSSL and libsrt linked in statically
#   <cache>/<key>/notices/ffmpeg/     licence texts + PROVENANCE.txt (versions, URLs,
#                                     SHA-256s, the exact configure lines)
# <cache> = $COREVIDEO_FFMPEG_CACHE or ~/Library/Caches/CoreVideoPro/ffmpeg-lgpl.
# <key>   = first 16 hex of SHA-256(macos-ffmpeg.lock + this script), so any pin
#           or flag change rebuilds and an unchanged pair reuses the build.
#
# Why a source build (owner decision 2026-11-18: bundle LGPL ffmpeg): every
# prebuilt macOS ffmpeg we could pin is GPL-configured (x264/x265), and
# Homebrew's is GPL plus dozens of dylibs from /opt/homebrew. This build is
# reproducible from three SHA-256-pinned upstream tarballs and needs only the
# Xcode command line tools, cmake, perl and curl. No Homebrew library can leak
# in: --disable-autodetect, and pkg-config is a shim that answers ONLY for the
# two dependencies built here.
#
# LGPL posture: --enable-version3 (the binary is LGPL v3 or later: OpenSSL 3 is
# Apache-2.0, which the FSF counts compatible with the v3 licences, and it is
# linked in statically; ffmpeg's configure only insists on version3 when GPL is
# on, so without the flag the result would claim LGPL 2.1+ around Apache code).
# No --enable-gpl, no --enable-nonfree; the audit refuses a
# binary whose `-L` is not LGPL or whose buildconf names either flag. The core
# runs ffmpeg as a separate process and never links it.
set -euo pipefail
cd "$(dirname "$0")/.."
ROOT="$(pwd)"
LOCK="$ROOT/scripts/macos-ffmpeg.lock"
SELF="$ROOT/scripts/build-macos-ffmpeg.sh"
die() { echo "error: $*" >&2; exit 1; }
log() { printf '==> %s\n' "$*" >&2; }

# ── audit: the questions a release must answer about the shipped binary ──────
audit() {  # audit <ffmpeg>
    local bin="$1" out fail=0
    [ -x "$bin" ] || die "audit: no executable at $bin"
    local archs; archs="$(lipo -archs "$bin" 2>/dev/null || true)"
    [ "$archs" = "arm64" ] || { echo "audit: architectures '$archs', want arm64 only" >&2; fail=1; }
    # -L wraps its text ("...GNU Lesser General Public\nLicense..."): join the lines.
    out="$("$bin" -hide_banner -L 2>&1 | tr '\n' ' ' | tr -s ' ' || true)"
    grep -q "GNU Lesser General Public License" <<<"$out" \
        || { echo "audit: ffmpeg -L does not report the LGPL" >&2; fail=1; }
    out="$("$bin" -hide_banner -buildconf 2>&1 || true)"
    if grep -qE -- '--enable-(gpl|nonfree)' <<<"$out"; then
        echo "audit: buildconf enables gpl or nonfree" >&2; fail=1
    fi
    # Only the OS: a dylib from /opt/homebrew or /usr/local would be missing on
    # a customer's Mac and could carry any licence.
    local libs; libs="$(otool -L "$bin" | tail -n +2 | awk '{print $1}')"
    while IFS= read -r lib; do
        [ -z "$lib" ] && continue
        case "$lib" in /usr/lib/*|/System/Library/*) ;; *) echo "audit: non-system library $lib" >&2; fail=1 ;; esac
    done <<<"$libs"
    # otool -L cannot see a STATIC library: also refuse any Homebrew/MacPorts
    # path baked into the binary, and an OpenSSL that is not the pinned one
    # (Homebrew's openssl@3 sits on this machine and would link just as well).
    # Capture first: `strings | grep -q` under pipefail turns a hit into a
    # SIGPIPE failure (the trap release-macos.sh warns about).
    local text; text="$(strings -a "$bin")"
    if grep -qE '/opt/homebrew|/usr/local/(opt|Cellar|lib)|/opt/local/' <<<"$text"; then
        echo "audit: binary embeds a Homebrew/MacPorts path" >&2; fail=1
    fi
    local pinned_openssl; pinned_openssl="$(sed -n 's/^OPENSSL_VERSION=//p' "$LOCK")"
    grep -qE "^OpenSSL ${pinned_openssl//./\\.} " <<<"$text" \
        || { echo "audit: embedded OpenSSL is not the pinned $pinned_openssl" >&2; fail=1; }
    out="$("$bin" -hide_banner -encoders 2>&1 || true)"
    for enc in h264_videotoolbox aac; do
        grep -qE "^ [A-Z.]{6} $enc( |$)" <<<"$out" || { echo "audit: missing encoder $enc" >&2; fail=1; }
    done
    out="$("$bin" -hide_banner -protocols 2>&1 || true)"
    for proto in rtmp rtmps srt https tls; do
        grep -qE "^ +$proto$" <<<"$out" || { echo "audit: missing protocol $proto" >&2; fail=1; }
    done
    out="$("$bin" -hide_banner -muxers 2>&1 || true)"
    for mux in flv mpegts hls; do
        grep -qE "^ +E +$mux " <<<"$out" || { echo "audit: missing muxer $mux" >&2; fail=1; }
    done
    out="$("$bin" -hide_banner -filters 2>&1 || true)"
    for filter in anullsrc aresample; do
        grep -qE " $filter " <<<"$out" || { echo "audit: missing filter $filter" >&2; fail=1; }
    done
    [ "$fail" = 0 ] || return 1
    echo "audit: $bin OK (arm64, LGPL, system libraries only, h264_videotoolbox/aac, rtmp/rtmps/srt/https, flv/mpegts/hls)" >&2
}

if [ "${1:-}" = "--audit" ]; then
    audit "${2:?--audit needs a path}"
    exit $?
fi
SOURCE_BUNDLE=""
if [ "${1:-}" = "--source-bundle" ]; then
    SOURCE_BUNDLE="${2:?--source-bundle needs a directory}"
    shift 2
fi
[ $# -eq 0 ] || die "unknown argument '$1'"

[ "$(uname -s)" = "Darwin" ] && [ "$(uname -m)" = "arm64" ] || die "build on an Apple Silicon Mac"
for tool in curl shasum tar cmake make perl clang lipo otool strings; do
    command -v "$tool" >/dev/null || die "missing tool: $tool"
done
# shellcheck source=macos-ffmpeg.lock
. "$LOCK"

CACHE="${COREVIDEO_FFMPEG_CACHE:-$HOME/Library/Caches/CoreVideoPro/ffmpeg-lgpl}"
KEY="$(cat "$LOCK" "$SELF" | shasum -a 256 | cut -c1-16)"
OUT="$CACHE/$KEY"
if [ -z "$SOURCE_BUNDLE" ] && [ -f "$OUT/BUILD_OK" ] && [ -x "$OUT/bin/ffmpeg" ]; then
    audit "$OUT/bin/ffmpeg" || die "cached build at $OUT fails the audit; delete it and rebuild"
    echo "$OUT"
    exit 0
fi

DOWNLOADS="$CACHE/downloads"
mkdir -p "$DOWNLOADS"
fetch() {  # fetch <url> <sha256> <file>
    local url="$1" want="$2" file="$DOWNLOADS/$3" got
    if [ -f "$file" ]; then
        got="$(shasum -a 256 "$file" | cut -d' ' -f1)"
        [ "$got" = "$want" ] && return 0
        rm -f "$file"
    fi
    log "download $url"
    curl -fsSL --retry 3 -o "$file.part" "$url"
    got="$(shasum -a 256 "$file.part" | cut -d' ' -f1)"
    [ "$got" = "$want" ] || { rm -f "$file.part"; die "SHA-256 mismatch for $url: got $got, pinned $want"; }
    mv "$file.part" "$file"
}
fetch "$FFMPEG_URL" "$FFMPEG_SHA256" "ffmpeg-$FFMPEG_VERSION.tar.xz"
fetch "$OPENSSL_URL" "$OPENSSL_SHA256" "openssl-$OPENSSL_VERSION.tar.gz"
fetch "$SRT_URL" "$SRT_SHA256" "srt-$SRT_VERSION.tar.gz"

if [ -n "$SOURCE_BUNDLE" ]; then
    # The corresponding source for the shipped binary: exactly what it was
    # built from (fetch re-verified every pin above), unmodified.
    mkdir -p "$SOURCE_BUNDLE"
    for f in "ffmpeg-$FFMPEG_VERSION.tar.xz" "openssl-$OPENSSL_VERSION.tar.gz" "srt-$SRT_VERSION.tar.gz"; do
        cp "$DOWNLOADS/$f" "$SOURCE_BUNDLE/$f"
    done
    cp "$SELF" "$SOURCE_BUNDLE/build-macos-ffmpeg.sh"
    cp "$LOCK" "$SOURCE_BUNDLE/macos-ffmpeg.lock"
    ( cd "$SOURCE_BUNDLE" && shasum -a 256 ffmpeg-* openssl-* srt-* build-macos-ffmpeg.sh macos-ffmpeg.lock >SHA256SUMS )
    echo "$SOURCE_BUNDLE"
    exit 0
fi

WORK="$(mktemp -d "${TMPDIR:-/tmp}/cvp-ffmpeg.XXXXXX")"
trap 'rm -rf "$WORK"' EXIT
DEPS="$WORK/deps"
STAGE="$WORK/stage"
mkdir -p "$DEPS" "$STAGE"
JOBS="$(sysctl -n hw.ncpu)"
export MACOSX_DEPLOYMENT_TARGET="$MACOS_MIN"
# Each archive gets its own empty directory (downloaded content is data, and a
# tarball must never extract over another).
unpack() {  # unpack <file> <dir>
    mkdir -p "$WORK/$2"
    tar -xf "$DOWNLOADS/$1" -C "$WORK/$2" --strip-components 1
}

log "OpenSSL $OPENSSL_VERSION (static, libcrypto + libssl only)"
unpack "openssl-$OPENSSL_VERSION.tar.gz" openssl
OPENSSL_CONFIGURE=(darwin64-arm64-cc no-shared no-tests no-docs no-apps no-module
    "--prefix=$DEPS" --libdir=lib "-mmacosx-version-min=$MACOS_MIN")
( cd "$WORK/openssl" && perl ./Configure "${OPENSSL_CONFIGURE[@]}" >/dev/null \
    && make -j "$JOBS" build_libs >/dev/null && make install_dev >/dev/null )

log "libsrt $SRT_VERSION (static, OpenSSL EVP encryption)"
unpack "srt-$SRT_VERSION.tar.gz" srt
SRT_CONFIGURE=(-DCMAKE_BUILD_TYPE=Release "-DCMAKE_INSTALL_PREFIX=$DEPS" -DCMAKE_INSTALL_LIBDIR=lib
    "-DCMAKE_OSX_DEPLOYMENT_TARGET=$MACOS_MIN" -DCMAKE_OSX_ARCHITECTURES=arm64
    -DENABLE_SHARED=OFF -DENABLE_STATIC=ON -DENABLE_APPS=OFF -DENABLE_ENCRYPTION=ON
    -DUSE_ENCLIB=openssl-evp "-DOPENSSL_ROOT_DIR=$DEPS" -DOPENSSL_USE_STATIC_LIBS=ON
    -DENABLE_UNITTESTS=OFF)
cmake -S "$WORK/srt" -B "$WORK/srt-build" "${SRT_CONFIGURE[@]}" >/dev/null
cmake --build "$WORK/srt-build" -j "$JOBS" >/dev/null
cmake --install "$WORK/srt-build" >/dev/null

# pkg-config shim: answers for exactly srt and openssl, from $DEPS. Anything
# else is "not found", so configure cannot discover a Homebrew library.
SHIM="$WORK/pkg-config"
cat >"$SHIM" <<SH
#!/bin/bash
mode=""; mods=()
# configure passes "srt >= 1.3.0" as ONE argument: split every argument into words.
for a in \$*; do
  case "\$a" in
    --exists|--cflags|--libs|--modversion) mode="\$a" ;;
    --*) ;;
    [\<\>=]*|[0-9]*) ;;
    *) mods+=("\$a") ;;
  esac
done
cflags="-I$DEPS/include"; libs=""
for m in "\${mods[@]}"; do
  case "\$m" in
    srt) libs="\$libs -L$DEPS/lib -lsrt -lssl -lcrypto -lc++"; ver=$SRT_VERSION ;;
    openssl|libssl) libs="\$libs -L$DEPS/lib -lssl -lcrypto"; ver=$OPENSSL_VERSION ;;
    libcrypto) libs="\$libs -L$DEPS/lib -lcrypto"; ver=$OPENSSL_VERSION ;;
    *) echo "pkg-config shim: \$m not provided" >&2; exit 1 ;;
  esac
done
case "\$mode" in
  --cflags) echo "\$cflags" ;;
  --libs) echo "\$libs" ;;
  --modversion) echo "\$ver" ;;
esac
exit 0
SH
chmod +x "$SHIM"

log "ffmpeg $FFMPEG_VERSION (LGPL, static, ffmpeg CLI only)"
unpack "ffmpeg-$FFMPEG_VERSION.tar.xz" ffmpeg
FFMPEG_CONFIGURE=("--prefix=$STAGE" --arch=arm64 --cc=clang
    --enable-static --disable-shared --disable-autodetect
    --disable-ffplay --disable-ffprobe --disable-doc --disable-debug
    --enable-videotoolbox --enable-audiotoolbox --enable-zlib
    --enable-openssl --enable-libsrt --enable-version3
    "--pkg-config=$SHIM" --pkg-config-flags=--static
    "--extra-cflags=-mmacosx-version-min=$MACOS_MIN"
    "--extra-ldflags=-mmacosx-version-min=$MACOS_MIN")
( cd "$WORK/ffmpeg" && ./configure "${FFMPEG_CONFIGURE[@]}" >"$WORK/ffmpeg-configure.log" 2>&1 ) \
    || { tail -40 "$WORK/ffmpeg-configure.log" >&2; tail -60 "$WORK/ffmpeg/ffbuild/config.log" >&2 || true; die "ffmpeg configure failed"; }
( cd "$WORK/ffmpeg" && make -j "$JOBS" >"$WORK/ffmpeg-make.log" 2>&1 && make install >/dev/null 2>&1 ) \
    || { tail -40 "$WORK/ffmpeg-make.log" >&2; die "ffmpeg build failed"; }
strip -x "$STAGE/bin/ffmpeg"
if ! audit "$STAGE/bin/ffmpeg"; then
    mkdir -p "$CACHE"; cp "$STAGE/bin/ffmpeg" "$CACHE/failed-audit-ffmpeg"
    die "the fresh build failed the audit (binary kept at $CACHE/failed-audit-ffmpeg)"
fi

# Licence texts + provenance (the Windows package ships notices/ffmpeg/ the same way).
N="$STAGE/notices/ffmpeg"
mkdir -p "$N"
cp "$WORK/ffmpeg/LICENSE.md" "$N/FFMPEG-LICENSE.md"
# LGPLv3 is a set of additional permissions ON the GPLv3, so the GPLv3 text
# is part of the licence and must ship with it (review finding 5).
for f in COPYING.LGPLv2.1 COPYING.LGPLv3 COPYING.GPLv3; do
    [ -f "$WORK/ffmpeg/$f" ] || die "ffmpeg source has no $f"
    cp "$WORK/ffmpeg/$f" "$N/FFMPEG-$f"
done
cp "$WORK/openssl/LICENSE.txt" "$N/OPENSSL-LICENSE.txt"
cp "$WORK/srt/LICENSE" "$N/LIBSRT-LICENSE-MPL-2.0.txt"
LICENSE_TEXT="$("$STAGE/bin/ffmpeg" -hide_banner -L 2>&1 | tr '\n' ' ' | tr -s ' ')"
case "$LICENSE_TEXT" in
    *"Lesser General Public License"*"version 2.1 of the License, or (at your option) any later"*)
        LICENSE_LINE="Licence: GNU LGPL version 2.1 or later (as reported by ffmpeg -L)" ;;
    *"Lesser General Public License"*"version 3 of the License"*)
        LICENSE_LINE="Licence: GNU LGPL version 3 or later (as reported by ffmpeg -L)" ;;
    *) die "ffmpeg -L names no LGPL version" ;;
esac
{
    echo "CoreVideo Pro for macOS bundles ffmpeg as a separate executable"
    echo "(Contents/Resources/ffmpeg). The media core runs it as a child process"
    echo "for streaming and does not link to it."
    echo
    echo "ffmpeg $FFMPEG_VERSION  $FFMPEG_URL"
    echo "  SHA-256 $FFMPEG_SHA256"
    echo "  $LICENSE_LINE"
    echo "  configure: ${FFMPEG_CONFIGURE[*]}" | sed "s|$WORK|<build>|g; s|$STAGE|<prefix>|g"
    echo "OpenSSL $OPENSSL_VERSION (Apache-2.0)  $OPENSSL_URL"
    echo "  SHA-256 $OPENSSL_SHA256"
    echo "  Configure: ${OPENSSL_CONFIGURE[*]}" | sed "s|$WORK|<build>|g"
    echo "libsrt $SRT_VERSION (MPL-2.0)  $SRT_URL"
    echo "  SHA-256 $SRT_SHA256"
    echo "  cmake: ${SRT_CONFIGURE[*]}" | sed "s|$WORK|<build>|g"
    echo
    echo "None of the three sources is modified. Rebuild with"
    echo "scripts/build-macos-ffmpeg.sh from the CoreVideo Pro source tree, or set"
    echo "COREVIDEO_FFMPEG_BIN_DIR to a directory holding a replacement ffmpeg."
} >"$N/PROVENANCE.txt"
{
    echo "Corresponding source for the bundled ffmpeg"
    echo
    echo "Contents/Resources/ffmpeg is ffmpeg $FFMPEG_VERSION built, unmodified, from the"
    echo "pinned sources listed in PROVENANCE.txt and licensed under the GNU LGPL"
    echo "version 3 or later (FFMPEG-COPYING.LGPLv3, which adds permissions to the GNU"
    echo "GPL version 3 in FFMPEG-COPYING.GPLv3). OpenSSL (Apache-2.0) and libsrt"
    echo "(MPL-2.0) are statically linked into that one executable."
    echo
    echo "Every CoreVideo Pro for macOS release on GitHub publishes, as assets of that"
    echo "same release, the complete corresponding source of the ffmpeg it ships:"
    echo
    echo "  ffmpeg-$FFMPEG_VERSION.tar.xz"
    echo "  openssl-$OPENSSL_VERSION.tar.gz"
    echo "  srt-$SRT_VERSION.tar.gz"
    echo "  build-macos-ffmpeg.sh   (the exact build script, configure lines included)"
    echo "  macos-ffmpeg.lock       (the pins and their SHA-256s)"
    echo "  SHA256SUMS"
    echo
    echo "  https://github.com/iamfatness/CoreVideoPro/releases"
    echo "  (choose the release whose version matches this app's About box)"
    echo
    echo "To use a modified or replacement ffmpeg, set COREVIDEO_FFMPEG_BIN_DIR to the"
    echo "directory holding it; the media core runs it as a separate process."
} >"$N/SOURCE.txt"

mkdir -p "$CACHE"
rm -rf "$OUT.tmp"
mv "$STAGE" "$OUT.tmp"
rm -rf "$OUT"
mv "$OUT.tmp" "$OUT"
date -u '+%Y-%m-%dT%H:%M:%SZ' >"$OUT/BUILD_OK"
log "built $OUT ($(du -h "$OUT/bin/ffmpeg" | cut -f1))"
echo "$OUT"
