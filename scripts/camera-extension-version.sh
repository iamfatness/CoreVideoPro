#!/bin/bash
# The camera extension's OWN version, and the gate that keeps it honest.
#
#   scripts/camera-extension-version.sh version     print CameraConstants.extensionVersion
#   scripts/camera-extension-version.sh check       exit 0 only if the record matches
#   scripts/camera-extension-version.sh update      re-record (refuses without a bump)
#   scripts/camera-extension-version.sh self-test   the negative test of this gate
#   options: --root <dir>         default: the repo root
#            --profile <path>     also compare the camera provisioning profile's UUID
#            --profile-uuid <id>  the same with the UUID given directly (self-test)
#
# WHY (owner requirement 2026-11-15, "no support tickets"): macOS replaces an
# installed system extension whenever the bundled CFBundleVersion differs, and
# a replacement while an app holds the camera can leave the old copy
# "terminated waiting to uninstall on reboot" and the new one with no device
# until a restart. The extension used to carry the app's build number, so
# EVERY app build replaced it. It now carries CameraConstants.extensionVersion,
# which changes only when the extension itself changes, and this gate is what
# makes "only when it changes" true. The flip side is just as important: an
# extension that differs under the SAME version is never installed on any Mac
# that already has that version. So everything that shapes the shipped
# extension bundle is recorded next to the version:
#
#   every file under mac-shell/Sources/CoreVideoProCamera
#   every file under mac-shell/Sources/CameraExtensionKit (the whole target is linked in)
#   scripts/macos-camera-extension.entitlements
#   the lines between "camera-extension-hashed" markers in
#     scripts/make-macos-shell-bundle.sh (the extension Info.plist) and
#     mac-shell/Package.swift (the CoreVideoProCamera target, linker settings)
#   the UUID of the embedded camera provisioning profile (--profile; release
#     only, CI has no profile): a renewed profile is a new extension bundle
#
# `check` fails when any of them changed but the version did not. Record:
# scripts/camera-extension-version.record (checked in). Run by
# scripts/release-macos.sh (preflight with --profile, and against the built
# bundle's embedded profile) and CI (mac-shell-design-lint, without a profile).
# The version started at 3000 (above the ~2883 build numbers the extension
# used to carry), so the transition replaced old installs as an upgrade.
set -uo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
CMD="${1:-}"; shift || true
PROFILE=""
PROFILE_UUID=""
while [ $# -gt 0 ]; do
    case "$1" in
        --root) ROOT="$2"; shift 2 ;;
        --profile) PROFILE="$2"; shift 2 ;;
        --profile-uuid) PROFILE_UUID="$2"; shift 2 ;;
        *) echo "unknown argument: $1" >&2; exit 2 ;;
    esac
done

CONSTANTS="mac-shell/Sources/CameraExtensionKit/CameraConstants.swift"
RECORD="scripts/camera-extension-version.record"
BLOCK_FILES="scripts/make-macos-shell-bundle.sh mac-shell/Package.swift"

sha256() {  # stdin -> hex digest
    if command -v shasum >/dev/null 2>&1; then shasum -a 256 | cut -d' ' -f1
    else sha256sum | cut -d' ' -f1; fi
}

current_version() {
    sed -n 's/^ *public static let extensionVersion = \([0-9][0-9]*\).*$/\1/p' "$ROOT/$CONSTANTS" | head -1
}

# Whole files that are hashed.
hashed_files() {
    ( cd "$ROOT" && {
        find mac-shell/Sources/CoreVideoProCamera mac-shell/Sources/CameraExtensionKit -type f \
            ! -name '.DS_Store' ! -name '*.bak'
        echo scripts/macos-camera-extension.entitlements
    } | LC_ALL=C sort )
}

# The marked block of <file>, or nothing when the markers are missing.
marked_block() {
    awk '/>>> camera-extension-hashed/{on=1} on{print} /<<< camera-extension-hashed/{on=0}' "$ROOT/$1"
}

# sha256 over "<sha256>  <name>" lines, so a rename or a new file counts too.
current_hash() {
    local f b
    {
        hashed_files | while IFS= read -r f; do
            [ -f "$ROOT/$f" ] || { echo "MISSING  $f"; continue; }
            printf '%s  %s\n' "$(sha256 <"$ROOT/$f")" "$f"
        done
        for f in $BLOCK_FILES; do
            b="$(marked_block "$f")"
            [ -n "$b" ] || { echo "MISSING-BLOCK  $f"; continue; }
            printf '%s  %s#camera-extension-hashed\n' "$(printf '%s\n' "$b" | sha256)" "$f"
        done
    } | sha256
}

check_blocks() {
    local f
    for f in $BLOCK_FILES; do
        [ -n "$(marked_block "$f")" ] || { echo "FAIL: no camera-extension-hashed markers in $f"; return 1; }
    done
}

# The camera profile's UUID from --profile-uuid or --profile, or empty.
profile_uuid() {
    if [ -n "$PROFILE_UUID" ]; then echo "$PROFILE_UUID"; return 0; fi
    [ -n "$PROFILE" ] || return 0
    local tmp; tmp="$(mktemp)"
    if ! security cms -D -i "$PROFILE" >"$tmp" 2>/dev/null; then
        rm -f "$tmp"; echo "FAIL: cannot decode the camera profile $PROFILE" >&2; return 1
    fi
    plutil -extract UUID raw -o - "$tmp" 2>/dev/null || { rm -f "$tmp"; echo "FAIL: no UUID in $PROFILE" >&2; return 1; }
    rm -f "$tmp"
}

recorded() {  # recorded <key>
    sed -n "s/^$1=//p" "$ROOT/$RECORD" 2>/dev/null | head -1
}

bump_advice() {
    echo "      Every installed copy is replaced only when this version changes, so a changed"
    echo "      extension MUST bump it: edit $CONSTANTS, then run"
    echo "      scripts/camera-extension-version.sh update [--profile <camera profile>]"
    echo "      and commit $RECORD."
}

do_check() {
    local v h rv rh ru pu
    check_blocks || return 1
    v="$(current_version)"; h="$(current_hash)"
    pu="$(profile_uuid)" || return 1
    [ -n "$v" ] || { echo "FAIL: no 'public static let extensionVersion = <int>' in $CONSTANTS"; return 1; }
    [ -f "$ROOT/$RECORD" ] || { echo "FAIL: no $RECORD (run: scripts/camera-extension-version.sh update)"; return 1; }
    rv="$(recorded extensionVersion)"; rh="$(recorded sourceHash)"; ru="$(recorded cameraProfileUUID)"
    if [ "$v" != "$rv" ]; then
        if [ "$v" -lt "$rv" ] 2>/dev/null; then
            echo "FAIL: CameraConstants.extensionVersion went backwards ($rv -> $v)."
            return 1
        fi
        echo "FAIL: CameraConstants.extensionVersion is $v but $RECORD records $rv."
        echo "      Run scripts/camera-extension-version.sh update and commit $RECORD."
        return 1
    fi
    if [ "$h" != "$rh" ]; then
        echo "FAIL: the camera extension's sources changed but CameraConstants.extensionVersion is still $v."
        bump_advice
        echo "      recorded $rh, now $h"
        return 1
    fi
    if [ -n "$pu" ]; then
        if [ -z "$ru" ]; then
            echo "FAIL: $RECORD records no camera profile UUID; run update --profile <camera profile>."
            return 1
        fi
        if [ "$pu" != "$ru" ]; then
            echo "FAIL: the camera provisioning profile changed (recorded $ru, now $pu) but"
            echo "      CameraConstants.extensionVersion is still $v. A renewed profile is a new"
            echo "      extension bundle that no Mac with version $v would ever install."
            bump_advice
            return 1
        fi
        echo "ok: camera extension version $v matches its sources ($h) and profile $pu"
    else
        echo "ok: camera extension version $v matches its sources ($h) (profile not checked)"
    fi
    return 0
}

do_update() {
    local v h rv rh ru pu
    check_blocks || return 1
    v="$(current_version)"; h="$(current_hash)"
    pu="$(profile_uuid)" || return 1
    [ -n "$v" ] || { echo "FAIL: no extensionVersion in $CONSTANTS"; return 1; }
    rv="$(recorded extensionVersion)"; rh="$(recorded sourceHash)"; ru="$(recorded cameraProfileUUID)"
    if [ -n "$rv" ]; then
        if [ "$v" -lt "$rv" ]; then echo "FAIL: extensionVersion went backwards ($rv -> $v)"; return 1; fi
        if [ "$v" = "$rv" ] && [ "$h" != "$rh" ]; then
            echo "FAIL: refusing to re-record changed sources under the same extensionVersion $v; bump it first."
            return 1
        fi
        if [ "$v" = "$rv" ] && [ -n "$pu" ] && [ -n "$ru" ] && [ "$pu" != "$ru" ]; then
            echo "FAIL: refusing to re-record a changed camera profile under the same extensionVersion $v; bump it first."
            return 1
        fi
    fi
    [ -n "$pu" ] || pu="$ru"   # no profile given: keep the recorded one
    {
        echo "# Written by scripts/camera-extension-version.sh update. Do not edit by hand."
        echo "# The camera extension's version, a sha256 of everything it is built from"
        echo "# (see the script header) and the UUID of its provisioning profile."
        echo "# extensionVersion started at 3000 (2026-11-15), above every app build number"
        echo "# earlier builds used as the extension's CFBundleVersion (the owner's Mac had"
        echo "# 2881 and 2883), so the move to an independent version is an upgrade, never a"
        echo "# downgrade. Only ever increase it. Renewing the camera profile needs a bump."
        echo "extensionVersion=$v"
        echo "sourceHash=$h"
        echo "cameraProfileUUID=$pu"
    } >"$ROOT/$RECORD"
    echo "recorded extensionVersion=$v sourceHash=$h cameraProfileUUID=$pu"
}

do_self_test() {
    local work self rc out f
    work="$(mktemp -d)"; trap 'rm -rf "$work"' RETURN
    self="$ROOT/scripts/camera-extension-version.sh"
    ( cd "$ROOT" && { hashed_files; for f in $BLOCK_FILES; do echo "$f"; done; echo "$RECORD"; } ) \
        | while IFS= read -r f; do
            mkdir -p "$work/$(dirname "$f")"; cp "$ROOT/$f" "$work/$f"
        done
    local ru; ru="$(recorded cameraProfileUUID)"; [ -n "$ru" ] || ru="00000000-0000-0000-0000-000000000000"
    expect() {  # expect <want-rc 0|1> <grep-pattern> <label> -- cmd...
        local want="$1" pat="$2" label="$3"; shift 4
        out="$("$@" 2>&1)"; rc=$?
        if { [ "$want" = 0 ] && [ $rc -ne 0 ]; } || { [ "$want" = 1 ] && [ $rc -eq 0 ]; }; then
            echo "SELF-TEST FAILED: $label (exit $rc)"; echo "$out"; return 1
        fi
        grep -q -- "$pat" <<<"$out" || { echo "SELF-TEST FAILED: $label: output lacks '$pat'"; echo "$out"; return 1; }
        echo "self-test ok: $label"
    }
    restore() { cp "$ROOT/$1" "$work/$1"; }
    expect 0 "^ok:" "the checked-in record matches" -- "$self" check --root "$work" || return 1
    if [ "$ru" != "00000000-0000-0000-0000-000000000000" ]; then
        expect 0 "and profile" "the recorded profile UUID matches" -- \
            "$self" check --root "$work" --profile-uuid "$ru" || return 1
    fi
    # Each input, changed without a bump, fails.
    echo "// a change to the extension" >>"$work/mac-shell/Sources/CoreVideoProCamera/SinkStream.swift"
    expect 1 "sources changed but CameraConstants.extensionVersion is still" \
        "a changed extension source without a bump fails" -- "$self" check --root "$work" || return 1
    restore mac-shell/Sources/CoreVideoProCamera/SinkStream.swift
    echo "resource" >"$work/mac-shell/Sources/CoreVideoProCamera/new-resource.txt"
    expect 1 "sources changed" "a new non-Swift file in the extension fails" -- "$self" check --root "$work" || return 1
    rm "$work/mac-shell/Sources/CoreVideoProCamera/new-resource.txt"
    echo "<!-- entitlement change -->" >>"$work/scripts/macos-camera-extension.entitlements"
    expect 1 "sources changed" "a changed entitlements file without a bump fails" -- "$self" check --root "$work" || return 1
    restore scripts/macos-camera-extension.entitlements
    sed -i.bak 's/Lets CoreVideo Pro offer its Program output/Lets CoreVideo Pro share its Program output/' \
        "$work/scripts/make-macos-shell-bundle.sh"
    expect 1 "sources changed" "a changed extension Info.plist block without a bump fails" -- \
        "$self" check --root "$work" || return 1
    restore scripts/make-macos-shell-bundle.sh
    echo "# an unrelated line outside the markers" >>"$work/scripts/make-macos-shell-bundle.sh"
    expect 0 "^ok:" "a change outside the marked block does not count" -- "$self" check --root "$work" || return 1
    restore scripts/make-macos-shell-bundle.sh
    sed -i.bak 's/.linkedFramework("VideoToolbox")/.linkedFramework("VideoToolbox"), .linkedFramework("Metal")/' \
        "$work/mac-shell/Package.swift"
    expect 1 "sources changed" "a changed extension target in Package.swift without a bump fails" -- \
        "$self" check --root "$work" || return 1
    restore mac-shell/Package.swift
    sed -i.bak 's/camera-extension-hashed/camera-extension-unmarked/' "$work/mac-shell/Package.swift"
    expect 1 "no camera-extension-hashed markers" "removed markers fail" -- "$self" check --root "$work" || return 1
    restore mac-shell/Package.swift
    expect 1 "camera provisioning profile changed" "a renewed profile without a bump fails" -- \
        "$self" check --root "$work" --profile-uuid "11111111-2222-3333-4444-555555555555" || return 1
    expect 1 "refusing to re-record a changed camera profile" "update refuses a renewed profile without a bump" -- \
        "$self" update --root "$work" --profile-uuid "11111111-2222-3333-4444-555555555555" || return 1
    # A bump re-records.
    echo "// a change to the extension" >>"$work/mac-shell/Sources/CoreVideoProCamera/SinkStream.swift"
    expect 1 "refusing to re-record" "update refuses changed sources without a bump" -- "$self" update --root "$work" || return 1
    local v; v="$(sed -n "s/^ *public static let extensionVersion = \([0-9][0-9]*\).*$/\1/p" "$work/$CONSTANTS")"
    sed -i.bak "s/^\( *public static let extensionVersion = \)[0-9][0-9]*/\1$((v + 1))/" "$work/$CONSTANTS"
    expect 1 "records $v" "a bump without re-recording fails" -- "$self" check --root "$work" || return 1
    expect 0 "recorded extensionVersion=$((v + 1))" "update records the bump" -- \
        "$self" update --root "$work" --profile-uuid "11111111-2222-3333-4444-555555555555" || return 1
    expect 0 "and profile" "the bumped record passes with the new profile" -- \
        "$self" check --root "$work" --profile-uuid "11111111-2222-3333-4444-555555555555" || return 1
    echo "PASS: camera extension version gate self-test"
}

case "$CMD" in
    version) v="$(current_version)"; [ -n "$v" ] || { echo "no extensionVersion in $CONSTANTS" >&2; exit 1; }; echo "$v" ;;
    hash) current_hash ;;
    check) do_check ;;
    update) do_update ;;
    self-test) do_self_test ;;
    *) echo "usage: $0 version|check|update|self-test [--root <dir>] [--profile <path>|--profile-uuid <uuid>]" >&2; exit 2 ;;
esac
