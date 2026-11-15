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

# verify_profile <profile> <expected-app-id> <min-days> [signed-code]
#
# A Developer ID provisioning profile is fit to embed when ALL of these hold
# (each failure is printed, and the function returns 1):
#   - Entitlements:com.apple.application-identifier = <expected-app-id>
#   - TeamIdentifier = 42394L3353
#   - the profile expires no sooner than <min-days> from now
#   - the signing certificate's SHA-1 is among its DeveloperCertificates
#   - that certificate ALSO expires no sooner than <min-days> from now. The
#     profiles run to 2044; the Developer ID certificate in them expires
#     2027-02-01, so the certificate is the clock that actually runs out.
# The certificate is the leaf of [signed-code] when given (a check on a built
# bundle), else the keychain certificate named by $SIGN_ID (before signing).
# A mismatched profile does not fail codesign; it fails activation silently.
verify_profile() {  # verify_profile <profile> <expected-app-id> <min-days> [signed-code]
    local profile="$1" want_id="$2" min_days="$3" code="${4:-}"
    local tmp plist got_id team exp exp_s now_s days cert_pem sha1 end end_s i found
    tmp="$(mktemp -d)"
    plist="$tmp/profile.plist"
    if [ ! -f "$profile" ]; then
        echo "verify_profile: no profile at $profile" >&2; rm -rf "$tmp"; return 1
    fi
    if ! security cms -D -i "$profile" >"$plist" 2>/dev/null; then
        echo "verify_profile: $profile is not a signed provisioning profile" >&2; rm -rf "$tmp"; return 1
    fi
    got_id="$(/usr/libexec/PlistBuddy -c 'Print :Entitlements:com.apple.application-identifier' "$plist" 2>/dev/null)"
    if [ "$got_id" != "$want_id" ]; then
        echo "verify_profile: $profile is for '$got_id', expected '$want_id'" >&2; rm -rf "$tmp"; return 1
    fi
    team="$(/usr/libexec/PlistBuddy -c 'Print :TeamIdentifier:0' "$plist" 2>/dev/null)"
    if [ "$team" != "42394L3353" ]; then
        echo "verify_profile: $profile TeamIdentifier is '$team', expected 42394L3353" >&2; rm -rf "$tmp"; return 1
    fi
    now_s="$(date -u +%s)"
    exp="$(plutil -extract ExpirationDate raw -o - "$plist" 2>/dev/null)"
    exp_s="$(date -j -u -f '%Y-%m-%dT%H:%M:%SZ' "$exp" +%s 2>/dev/null)" || exp_s=0
    days=$(( (exp_s - now_s) / 86400 ))
    if [ "$exp_s" -le "$now_s" ] || [ "$days" -lt "$min_days" ]; then
        echo "verify_profile: $profile expires $exp ($days days left, need $min_days)" >&2; rm -rf "$tmp"; return 1
    fi
    cert_pem="$tmp/cert.pem"
    if [ -n "$code" ]; then
        case "$code" in /*) ;; *) code="$PWD/$code" ;; esac
        if ! (cd "$tmp" && codesign -d --extract-certificates=leaf "$code" >/dev/null 2>&1) \
            || ! openssl x509 -inform DER -in "$tmp/leaf0" -out "$cert_pem" 2>/dev/null; then
            echo "verify_profile: cannot read the signing certificate of $code" >&2; rm -rf "$tmp"; return 1
        fi
    else
        # The VALID identity of that exact name (an expired or revoked
        # certificate can share the name), then its certificate by SHA-1.
        local want_sha pem
        want_sha="$(security find-identity -v -p codesigning 2>/dev/null \
            | grep -F "\"$SIGN_ID\"" | awk 'NR==1 {print toupper($2)}')"
        security find-certificate -a -c "$SIGN_ID" -p 2>/dev/null \
            | awk -v d="$tmp" '/BEGIN CERT/ {n++} {print > (d "/kc" n ".pem")}'
        for pem in "$tmp"/kc*.pem; do
            [ -f "$pem" ] || continue
            if [ "$(openssl x509 -in "$pem" -noout -fingerprint -sha1 2>/dev/null \
                    | sed 's/.*=//; s/://g' | tr 'a-f' 'A-F')" = "$want_sha" ]; then
                cp "$pem" "$cert_pem"
            fi
        done
        if [ -z "$want_sha" ] || [ ! -s "$cert_pem" ]; then
            echo "verify_profile: no valid keychain identity '$SIGN_ID'" >&2; rm -rf "$tmp"; return 1
        fi
    fi
    sha1="$(openssl x509 -in "$cert_pem" -noout -fingerprint -sha1 2>/dev/null | sed 's/.*=//; s/://g' | tr 'a-f' 'A-F')"
    end="$(openssl x509 -in "$cert_pem" -noout -enddate 2>/dev/null | sed 's/^notAfter=//')"
    end_s="$(date -j -u -f '%b %e %T %Y %Z' "$end" +%s 2>/dev/null)" || end_s=0
    days=$(( (end_s - now_s) / 86400 ))
    if [ "$end_s" -le "$now_s" ] || [ "$days" -lt "$min_days" ]; then
        echo "verify_profile: signing certificate $sha1 expires '$end' ($days days left, need $min_days)" >&2
        rm -rf "$tmp"; return 1
    fi
    found=""; i=0
    while /usr/libexec/PlistBuddy -x -c "Print :DeveloperCertificates:$i" "$plist" >"$tmp/c.xml" 2>/dev/null; do
        if [ "$(sed -n '/<data>/,/<\/data>/p' "$tmp/c.xml" | sed 's/<\/*data>//g' | tr -d ' \t\n' \
                | base64 -D 2>/dev/null | shasum -a 1 | awk '{print toupper($1)}')" = "$sha1" ]; then
            found=1; break
        fi
        i=$((i + 1))
    done
    if [ -z "$found" ]; then
        echo "verify_profile: signing certificate $sha1 is not in $profile DeveloperCertificates ($i listed)" >&2
        rm -rf "$tmp"; return 1
    fi
    echo "verify_profile: ok $want_id (profile $exp, certificate $sha1 to '$end', $days days)"
    rm -rf "$tmp"
}
