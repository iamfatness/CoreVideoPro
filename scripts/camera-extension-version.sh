#!/bin/bash
# The camera extension's OWN version, and the gate that keeps it honest.
#
#   scripts/camera-extension-version.sh version     print CameraConstants.extensionVersion
#   scripts/camera-extension-version.sh check       exit 0 only if the record matches
#   scripts/camera-extension-version.sh update      re-record (refuses without a bump)
#   scripts/camera-extension-version.sh self-test   the negative test of this gate
#   (any subcommand also takes --root <dir>; default: the repo root)
#
# WHY (owner requirement 2026-11-15, "no support tickets"): macOS replaces an
# installed system extension whenever the bundled CFBundleVersion differs, and
# a replacement while an app holds the camera can leave the old copy
# "terminated waiting to uninstall on reboot" and the new one with no device
# until a restart. The extension used to carry the app's build number, so
# EVERY app build replaced it. It now carries CameraConstants.extensionVersion,
# which changes only when the extension itself changes, and this gate is what
# makes "only when it changes" true: a sha256 over the sources the extension
# is built from is recorded next to the version, and `check` fails when the
# sources changed but the version did not.
#
# Hashed (the extension binary's inputs; keep in step with Package.swift):
#   mac-shell/Sources/CoreVideoProCamera/*.swift
#   mac-shell/Sources/CameraExtensionKit/*.swift   (the whole target is linked in)
#   scripts/macos-camera-extension.entitlements
# Record: scripts/camera-extension-version.record (checked in).
# Run by: scripts/release-macos.sh (preflight) and CI (mac-shell-design-lint).
set -uo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
CMD="${1:-}"; shift || true
while [ $# -gt 0 ]; do
    case "$1" in
        --root) ROOT="$2"; shift 2 ;;
        *) echo "unknown argument: $1" >&2; exit 2 ;;
    esac
done

CONSTANTS="mac-shell/Sources/CameraExtensionKit/CameraConstants.swift"
RECORD="scripts/camera-extension-version.record"

sha256() {  # stdin -> hex digest
    if command -v shasum >/dev/null 2>&1; then shasum -a 256 | cut -d' ' -f1
    else sha256sum | cut -d' ' -f1; fi
}

current_version() {
    sed -n 's/^ *public static let extensionVersion = \([0-9][0-9]*\).*$/\1/p' "$ROOT/$CONSTANTS" | head -1
}

hashed_files() {
    ( cd "$ROOT" && {
        find mac-shell/Sources/CoreVideoProCamera mac-shell/Sources/CameraExtensionKit \
            -type f -name '*.swift'
        echo scripts/macos-camera-extension.entitlements
    } | LC_ALL=C sort )
}

# sha256 over "<file sha256>  <path>" lines, so a rename or a new file counts too.
current_hash() {
    local f
    hashed_files | while IFS= read -r f; do
        [ -f "$ROOT/$f" ] || { echo "MISSING  $f"; continue; }
        printf '%s  %s\n' "$(sha256 <"$ROOT/$f")" "$f"
    done | sha256
}

recorded() {  # recorded <key>
    sed -n "s/^$1=//p" "$ROOT/$RECORD" 2>/dev/null | head -1
}

do_check() {
    local v h rv rh
    v="$(current_version)"; h="$(current_hash)"
    [ -n "$v" ] || { echo "FAIL: no 'public static let extensionVersion = <int>' in $CONSTANTS"; return 1; }
    [ -f "$ROOT/$RECORD" ] || { echo "FAIL: no $RECORD (run: scripts/camera-extension-version.sh update)"; return 1; }
    rv="$(recorded extensionVersion)"; rh="$(recorded sourceHash)"
    if [ "$v" = "$rv" ] && [ "$h" = "$rh" ]; then
        echo "ok: camera extension version $v matches its sources ($h)"
        return 0
    fi
    if [ "$v" = "$rv" ]; then
        echo "FAIL: the camera extension's sources changed but CameraConstants.extensionVersion is still $v."
        echo "      Every installed copy is replaced only when this version changes, so a changed"
        echo "      extension MUST bump it: edit $CONSTANTS, then run"
        echo "      scripts/camera-extension-version.sh update and commit $RECORD."
        echo "      recorded $rh, now $h"
        return 1
    fi
    if [ "$v" -lt "$rv" ] 2>/dev/null; then
        echo "FAIL: CameraConstants.extensionVersion went backwards ($rv -> $v)."
        return 1
    fi
    echo "FAIL: CameraConstants.extensionVersion is $v but $RECORD records $rv."
    echo "      Run scripts/camera-extension-version.sh update and commit $RECORD."
    return 1
}

do_update() {
    local v h rv rh
    v="$(current_version)"; h="$(current_hash)"
    [ -n "$v" ] || { echo "FAIL: no extensionVersion in $CONSTANTS"; return 1; }
    rv="$(recorded extensionVersion)"; rh="$(recorded sourceHash)"
    if [ -n "$rv" ]; then
        if [ "$v" -lt "$rv" ]; then echo "FAIL: extensionVersion went backwards ($rv -> $v)"; return 1; fi
        if [ "$h" != "$rh" ] && [ "$v" = "$rv" ]; then
            echo "FAIL: refusing to re-record changed sources under the same extensionVersion $v; bump it first."
            return 1
        fi
    fi
    {
        echo "# Written by scripts/camera-extension-version.sh update. Do not edit by hand."
        echo "# The camera extension's version and a sha256 of the sources it is built from."
        echo "extensionVersion=$v"
        echo "sourceHash=$h"
    } >"$ROOT/$RECORD"
    echo "recorded extensionVersion=$v sourceHash=$h"
}

do_self_test() {
    local work self rc out
    work="$(mktemp -d)"; trap 'rm -rf "$work"' RETURN
    self="$ROOT/scripts/camera-extension-version.sh"
    ( cd "$ROOT" && { hashed_files; echo "$RECORD"; } ) | while IFS= read -r f; do
        mkdir -p "$work/$(dirname "$f")"; cp "$ROOT/$f" "$work/$f"
    done
    expect() {  # expect <want-rc 0|1> <grep-pattern> <label> -- cmd...
        local want="$1" pat="$2" label="$3"; shift 4
        out="$("$@" 2>&1)"; rc=$?
        if { [ "$want" = 0 ] && [ $rc -ne 0 ]; } || { [ "$want" = 1 ] && [ $rc -eq 0 ]; }; then
            echo "SELF-TEST FAILED: $label (exit $rc)"; echo "$out"; return 1
        fi
        grep -q -- "$pat" <<<"$out" || { echo "SELF-TEST FAILED: $label: output lacks '$pat'"; echo "$out"; return 1; }
        echo "self-test ok: $label"
    }
    expect 0 "^ok:" "the checked-in record matches" -- "$self" check --root "$work" || return 1
    echo "// a change to the extension" >>"$work/mac-shell/Sources/CoreVideoProCamera/SinkStream.swift"
    expect 1 "sources changed but CameraConstants.extensionVersion is still" \
        "a changed extension source without a bump fails" -- "$self" check --root "$work" || return 1
    expect 1 "refusing to re-record" "update refuses without a bump" -- "$self" update --root "$work" || return 1
    local v; v="$(sed -n "s/^ *public static let extensionVersion = \([0-9][0-9]*\).*$/\1/p" "$work/$CONSTANTS")"
    sed -i.bak "s/^\( *public static let extensionVersion = \)[0-9][0-9]*/\1$((v + 1))/" "$work/$CONSTANTS"
    expect 1 "records $v" "a bump without re-recording fails" -- "$self" check --root "$work" || return 1
    expect 0 "recorded extensionVersion=$((v + 1))" "update records the bump" -- "$self" update --root "$work" || return 1
    expect 0 "^ok:" "the bumped record passes" -- "$self" check --root "$work" || return 1
    echo "<!-- entitlement change -->" >>"$work/scripts/macos-camera-extension.entitlements"
    expect 1 "sources changed" "a changed entitlements file without a bump fails" -- "$self" check --root "$work" || return 1
    echo "PASS: camera extension version gate self-test"
}

case "$CMD" in
    version) v="$(current_version)"; [ -n "$v" ] || { echo "no extensionVersion in $CONSTANTS" >&2; exit 1; }; echo "$v" ;;
    hash) current_hash ;;
    check) do_check ;;
    update) do_update ;;
    self-test) do_self_test ;;
    *) echo "usage: $0 version|check|update|self-test [--root <dir>]" >&2; exit 2 ;;
esac
