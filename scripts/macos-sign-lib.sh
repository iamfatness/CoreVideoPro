# Sourced by make-macos-shell-bundle.sh and make-macos-engine-bundle.sh.
# Ported from ~/Developer/CoreVideo/scripts/make-macos-bundle.sh (the plugin
# recipe that is proven through notarization).
#
# Contract: set SIGN_ID before sourcing ("-" = ad-hoc). With a real identity,
# every signature gets the hardened runtime and a secure timestamp. Ad-hoc
# signing cannot take --timestamp offline, so the ad-hoc path adds neither.
#
# macOS ships bash 3.2: an EMPTY array expanded under `set -u` is an unbound
# variable error there, hence the ${arr[@]+"${arr[@]}"} form. Do not simplify.

CS_HARDENED=()
if [ "$SIGN_ID" != "-" ]; then
    CS_HARDENED=(--options runtime --timestamp)
fi

sign_one() {  # sign_one <path> [extra codesign args...]
    local path="$1"; shift
    codesign --force --sign "$SIGN_ID" \
        ${CS_HARDENED[@]+"${CS_HARDENED[@]}"} "$@" "$path"
}

# True when <file> is a Mach-O image (thin or universal), by magic number.
# `file` is too slow to run over the ~600 MB Zoom SDK tree twice.
is_macho() {  # is_macho <file>
    local magic
    magic="$(head -c 4 "$1" 2>/dev/null | od -An -tx1 | tr -d ' \n')"
    case "$magic" in
        feedface|feedfacf|cefaedfe|cffaedfe|cafebabe|bebafeca) return 0 ;;
    esac
    return 1
}

# True when <file> is the CFBundleExecutable of the bundle that contains it
# (X.app/Contents/MacOS/<exe>, X.bundle/Contents/MacOS/<exe>,
# X.framework/Versions/<v>/<exe>). Those are signed WITH their container, so
# signing them on their own first would only be overwritten (and cost one
# timestamp round-trip each). Anything else that is Mach-O is a "bare"
# executable as far as codesign is concerned and needs its own signature.
is_bundle_main_exe() {  # is_bundle_main_exe <file>
    local f="$1" dir name plist exe
    dir="$(dirname "$f")"; name="$(basename "$f")"
    case "$dir" in
        */Contents/MacOS) plist="$(dirname "$dir")/Info.plist" ;;
        *.framework/Versions/*) plist="$dir/Resources/Info.plist" ;;
        *) return 1 ;;
    esac
    [ -f "$plist" ] || return 1
    exe="$(/usr/libexec/PlistBuddy -c 'Print :CFBundleExecutable' "$plist" 2>/dev/null)" \
        || return 1
    [ "$exe" = "$name" ]
}

# Sign every nested framework/bundle/dylib/app AND every bare Mach-O file
# (a helper tool inside a framework's Resources, a plugin .so, an executable
# in Contents/MacOS that is not the bundle's main one, ...) under <dir>,
# deepest first. Notarization rejects ANY Mach-O without its own Developer ID
# signature + hardened runtime + timestamp; the bundle-name patterns alone
# skip bare executables. Signing a container seals its contents, so anything
# signed inside it afterwards would invalidate that seal; `find -depth`
# yields a directory's files and subdirectories before the directory itself.
# Do not replace with a plain find. Symlinks are skipped (-type f / -type d):
# a framework's Versions/Current alias must not be signed twice.
sign_nested() {  # sign_nested <dir>
    local n=0 bare=0 item
    while IFS= read -r item; do
        if [ -d "$item" ]; then
            case "$item" in
                *.framework|*.bundle|*.app|*.xpc|*.plugin|*.appex) ;;
                *) continue ;;
            esac
        else
            case "$item" in
                *.dylib|*.so) ;;
                *) is_macho "$item" || continue
                   is_bundle_main_exe "$item" && continue
                   bare=$((bare + 1)) ;;
            esac
        fi
        sign_one "$item"
        n=$((n + 1))
    done < <(find "$1" -depth \( -type d -o -type f \))
    echo "signed $n nested items ($bare bare Mach-O) under $1"
}
