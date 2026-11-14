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

# Sign every nested framework/bundle/dylib/app under <dir>, deepest first.
# Signing a container seals its contents, so anything signed inside it
# afterwards would invalidate that seal; `find -depth` yields contents before
# containers. Do not replace with a plain find.
sign_nested() {  # sign_nested <dir>
    local n=0 item
    while IFS= read -r item; do
        sign_one "$item"
        n=$((n + 1))
    done < <(find "$1" -depth \( -name "*.framework" -o -name "*.bundle" \
                -o -name "*.dylib" -o -name "*.app" \))
    echo "signed $n nested items under $1"
}
