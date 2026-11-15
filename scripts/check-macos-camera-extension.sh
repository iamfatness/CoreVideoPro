#!/bin/bash
# Structural check of the CoreMediaIO camera extension inside CoreVideo Pro.app
# (roadmap M9.2, docs/virtual-camera-macos-spec.md).
#
#   scripts/check-macos-camera-extension.sh [--release] <app>
#   scripts/check-macos-camera-extension.sh --negatives <app>
#
# Exit 0 = pass. Non-zero = fail, and the failing check is printed as
# "FAIL (x): ..." where x is the check letter below.
#
#   (a) the extension directory exists at the exact path
#   (b) the extension Info.plist keys and values
#   (c) extension CFBundleVersion equals the app's
#   (d) the mach service name starts with one of the extension's signed
#       com.apple.security.application-groups values
#   (e) the extension's signed entitlements include the app sandbox
#   (f) --release only: Developer ID authority + hardened runtime on the
#       extension, an embedded.provisionprofile in both bundles that passes
#       verify_profile with 14 days of margin (profile AND certificate), and
#       the app signed with com.apple.developer.system-extension.install
#   (g) no signed code in the bundle carries com.apple.application-identifier
#       (or any other restricted entitlement) unless its bundle has an
#       embedded.provisionprofile, and that profile covers every restricted
#       entitlement the code is signed with. AMFI kills a binary at launch
#       that is signed with a restricted entitlement no profile authorizes;
#       that is Review Focus 4 of the M9.2 plan.
#
# --negatives copies the bundle three times, breaks one thing in each copy,
# and requires the check to fail AT THE EXPECTED LETTER each time. It is the
# proof that (b), (c) and (g) actually look at the structure (plan rule 10).
set -uo pipefail
cd "$(dirname "$0")/.."

EXT_ID="us.iamfatness.corevideopro.shell.camera"
APP_ID="us.iamfatness.corevideopro.shell"
TEAM="42394L3353"
MACH_SERVICE="$TEAM.$EXT_ID"
DEVID_AUTHORITY="Developer ID Application: John Wallace ($TEAM)"
USAGE="Lets CoreVideo Pro offer its Program output as a camera in Zoom, Teams, FaceTime and other apps."

MODE="dev"
case "${1:-}" in
    --release) MODE="release"; shift ;;
    --negatives) MODE="negatives"; shift ;;
esac
APP="${1:-}"
[ -n "$APP" ] || { echo "usage: $0 [--release|--negatives] <app>" >&2; exit 2; }
APP="${APP%/}"
EXT="$APP/Contents/Library/SystemExtensions/$EXT_ID.systemextension"

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

fail() { echo "FAIL ($1): $2"; exit 1; }
ok() { echo "ok   ($1): $2"; }

# ── --negatives ─────────────────────────────────────────────────────────────
if [ "$MODE" = "negatives" ]; then
    [ -d "$EXT" ] || { echo "FAIL: no extension in $APP to break" >&2; exit 1; }
    negative() {  # negative <name> <expected-letter> <mutator-function>
        local name="$1" letter="$2" mutate="$3" copy out rc
        copy="$WORK/$name/CoreVideo Pro.app"
        mkdir -p "$WORK/$name"
        ditto "$APP" "$copy"
        "$mutate" "$copy/Contents/Library/SystemExtensions/$EXT_ID.systemextension"
        out="$("$0" "$copy" 2>&1)"; rc=$?
        if [ $rc -eq 0 ]; then
            echo "NEGATIVE FAILED: '$name' passed the check"; echo "$out"; exit 1
        fi
        if ! printf '%s\n' "$out" | grep -q "^FAIL ($letter)"; then
            echo "NEGATIVE FAILED: '$name' failed, but not at ($letter):"; echo "$out"; exit 1
        fi
        echo "negative ok: '$name' -> $(printf '%s\n' "$out" | grep '^FAIL' | head -1)"
    }
    mut_version() { plutil -replace CFBundleVersion -string 0 "$1/Contents/Info.plist"; }
    mut_restricted() {
        codesign --force --sign - \
            --entitlements scripts/macos-camera-extension.entitlements "$1" 2>/dev/null
    }
    mut_cmio() { plutil -remove CMIOExtension "$1/Contents/Info.plist"; }
    negative "build-number-mismatch" c mut_version
    negative "restricted-entitlements-ad-hoc" g mut_restricted
    negative "no-CMIOExtension" b mut_cmio
    echo "PASS: all 3 negatives were caught"
    exit 0
fi

# ── helpers ─────────────────────────────────────────────────────────────────
pb() { /usr/libexec/PlistBuddy -c "Print :$2" "$1" 2>/dev/null; }

# Signed entitlements of <code> as an XML plist file; empty file when none.
entitlements_of() {  # entitlements_of <code> <out>
    codesign -d --entitlements :- "$1" >"$2" 2>/dev/null || : >"$2"
    if [ -s "$2" ]; then plutil -convert xml1 "$2" 2>/dev/null || : >"$2"; fi
}
# Top-level keys of an XML plist dict (one tab of indent in plutil's output).
top_keys() { sed -n 's/^	<key>\(.*\)<\/key>$/\1/p' "$1"; }
# Restricted = needs a provisioning profile on macOS. Sandbox, app groups
# (team-prefixed), hardened-runtime cs.* and device.* are not.
is_restricted() {
    case "$1" in
        com.apple.application-identifier|com.apple.developer.*|keychain-access-groups) return 0 ;;
    esac
    return 1
}
# Scalar or array elements of key <k> in plist <f>, one per line.
values_of() {  # values_of <plist> <keypath>
    local raw
    raw="$(pb "$1" "$2")" || return 1
    case "$raw" in
        "Array {"*) printf '%s\n' "$raw" | sed '1d;$d' | sed 's/^ *//' ;;
        *) printf '%s\n' "$raw" ;;
    esac
}
# Profile value <allowed> authorizes signed value <v> (exact, or "X.*" prefix).
authorizes() {  # authorizes <allowed> <v>
    [ "$1" = "$2" ] && return 0
    case "$1" in *'*') [ "${2#"${1%\*}"}" != "$2" ] && return 0 ;; esac
    return 1
}

# ── (a) ─────────────────────────────────────────────────────────────────────
[ -d "$EXT" ] || fail a "no extension at $EXT"
ok a "extension at Contents/Library/SystemExtensions/$EXT_ID.systemextension"

# ── (b) ─────────────────────────────────────────────────────────────────────
EP="$EXT/Contents/Info.plist"
AP="$APP/Contents/Info.plist"
[ -f "$EP" ] || fail b "no Info.plist in the extension"
VERSION="$(pb "$AP" CFBundleShortVersionString)"
BUILD="$(pb "$AP" CFBundleVersion)"
want() {  # want <keypath> <value>
    local got
    got="$(pb "$EP" "$1")" || fail b "extension Info.plist has no $1"
    [ "$got" = "$2" ] || fail b "extension $1 is '$got', expected '$2'"
}
want CFBundleExecutable CoreVideoProCamera
want CFBundleIdentifier "$EXT_ID"
want CFBundleName "CoreVideo Pro Camera"
want CFBundlePackageType SYSX
want CFBundleShortVersionString "$VERSION"
want LSMinimumSystemVersion 13.0
want CMIOExtension:CMIOExtensionMachServiceName "$MACH_SERVICE"
want NSSystemExtensionUsageDescription "$USAGE"
[ -x "$EXT/Contents/MacOS/CoreVideoProCamera" ] || fail b "no executable Contents/MacOS/CoreVideoProCamera"
ok b "extension Info.plist keys and values"

# ── (c) ─────────────────────────────────────────────────────────────────────
EXT_BUILD="$(pb "$EP" CFBundleVersion)"
[ -n "$BUILD" ] && [ "$EXT_BUILD" = "$BUILD" ] || \
    fail c "extension CFBundleVersion '$EXT_BUILD' != app CFBundleVersion '$BUILD'"
ok c "CFBundleVersion $BUILD on both"

# ── (d) ─────────────────────────────────────────────────────────────────────
EXT_ENT="$WORK/ext-ent.plist"
entitlements_of "$EXT" "$EXT_ENT"
[ -s "$EXT_ENT" ] || fail d "extension has no signed entitlements"
GROUP_MATCH=""
while IFS= read -r g; do
    [ -n "$g" ] || continue
    case "$MACH_SERVICE" in "$g"*) GROUP_MATCH="$g" ;; esac
done < <(values_of "$EXT_ENT" com.apple.security.application-groups)
[ -n "$GROUP_MATCH" ] || fail d "mach service $MACH_SERVICE starts with no application-groups entitlement value"
ok d "mach service is inside app group $GROUP_MATCH"

# ── (e) ─────────────────────────────────────────────────────────────────────
[ "$(pb "$EXT_ENT" com.apple.security.app-sandbox)" = "true" ] || \
    fail e "extension is not signed with com.apple.security.app-sandbox"
ok e "extension is sandboxed"

# ── (f) ─────────────────────────────────────────────────────────────────────
if [ "$MODE" = "release" ]; then
    SIGN_ID="$DEVID_AUTHORITY"
    . scripts/macos-sign-lib.sh
    INFO="$(codesign -dvv "$EXT" 2>&1)"
    printf '%s\n' "$INFO" | grep -qx "Authority=$DEVID_AUTHORITY" || \
        fail f "extension is not signed by $DEVID_AUTHORITY"
    printf '%s\n' "$INFO" | grep -q '^CodeDirectory.*flags=.*(runtime)' || \
        fail f "extension lacks the hardened runtime flag"
    for pair in "$APP|$TEAM.$APP_ID" "$EXT|$MACH_SERVICE"; do
        b="${pair%%|*}"; id="${pair#*|}"
        [ -f "$b/Contents/embedded.provisionprofile" ] || \
            fail f "no embedded.provisionprofile in $(basename "$b")"
        vp="$(verify_profile "$b/Contents/embedded.provisionprofile" "$id" 14 "$b" 2>&1)" || \
            fail f "embedded.provisionprofile in $(basename "$b") fails verify_profile (14 days): $(printf '%s\n' "$vp" | tail -1)"
    done
    APP_ENT="$WORK/app-ent.plist"
    entitlements_of "$APP" "$APP_ENT"
    [ "$(pb "$APP_ENT" com.apple.developer.system-extension.install)" = "true" ] || \
        fail f "app is not signed with com.apple.developer.system-extension.install"
    ok f "Developer ID + runtime on the extension, both profiles verified (14 days), app may install system extensions"
fi

# ── (g) ─────────────────────────────────────────────────────────────────────
# Every bundle and bare Mach-O we sign ourselves: the app, the extension, the
# helpers in Resources and the engine app. (The engine's SDK frameworks are
# third-party and carry no entitlements.)
CODES=("$APP" "$EXT")
for c in "$APP/Contents/Resources/corevideo-native" \
         "$APP/Contents/Resources/corevideo-plugin-host" \
         "$APP/Contents/Resources/corevideo-zoom-engine.app"; do
    [ -e "$c" ] && CODES+=("$c")
done
n=0
for code in "${CODES[@]}"; do
    ent="$WORK/g-$n.plist"; n=$((n + 1))
    entitlements_of "$code" "$ent"
    [ -s "$ent" ] || continue
    restricted=()
    while IFS= read -r k; do
        is_restricted "$k" && restricted+=("$k")
    done < <(top_keys "$ent")
    [ ${#restricted[@]} -gt 0 ] || continue
    name="$(basename "$code")"
    prof="$code/Contents/embedded.provisionprofile"
    [ -d "$code" ] && [ -f "$prof" ] || \
        fail g "$name is signed with ${restricted[*]} but has no embedded.provisionprofile (AMFI kills it at launch)"
    security cms -D -i "$prof" >"$WORK/g-prof-raw-$n.plist" 2>/dev/null || \
        fail g "$name: embedded.provisionprofile does not decode"
    for k in "${restricted[@]}"; do
        while IFS= read -r v; do
            [ -n "$v" ] || continue
            covered=""
            while IFS= read -r allowed; do
                [ -n "$allowed" ] || continue
                authorizes "$allowed" "$v" && covered=1
            done < <(values_of "$WORK/g-prof-raw-$n.plist" "Entitlements:$k")
            [ -n "$covered" ] || fail g "$name is signed with $k=$v, which its embedded.provisionprofile does not authorize"
        done < <(values_of "$ent" "$k")
    done
done
ok g "every restricted entitlement is covered by an embedded.provisionprofile"
echo "PASS ($MODE): $APP"
