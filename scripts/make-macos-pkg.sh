#!/bin/bash
# Build the CoreVideo Pro macOS .pkg installer from an already-built, signed
# (and, for a release, notarized + stapled) "CoreVideo Pro.app". Does not
# build or modify the app; the app's code signature and stapled ticket ride
# through unchanged (pkgbuild --component copies the bundle byte for byte).
#
#   scripts/make-macos-pkg.sh --app PATH --version X.Y.Z --out FILE.pkg [--sign IDENTITY]
#
#   --app PATH       the .app to package (read-only input)
#   --version X.Y.Z  must equal the app's CFBundleShortVersionString (hard error)
#   --out FILE       output product-archive path
#   --sign IDENTITY  "Developer ID Installer: ..." identity. Default:
#                    $COREVIDEO_PKG_SIGN_IDENTITY, else
#                    "Developer ID Installer: John Wallace (42394L3353)".
#                    This is a DIFFERENT certificate from the Developer ID
#                    Application one that signs the app; an identity whose name
#                    lacks "Developer ID Installer" is refused.
#
# Installs to /Applications (system domain, admin auth), arm64 only,
# macOS 13+ (the app's LSMinimumSystemVersion). Also installs a byte-identical
# copy of the core at /Library/Application Support/CoreVideo Pro/corevideo-native:
# the virtual camera extension verifies its producer by reading the producer's
# code on disk, and its sandbox reads /Library but not /Applications (a sandbox
# exception cannot fix that on macOS 26.3; see ProducerAttestation.swift). The
# shell runs that copy only while it matches the app's own (CoreLocation.swift).
# The .pkg is the only macOS installer: a drag-installed app has no such copy.
set -euo pipefail

APP=""; VERSION=""; OUT=""
SIGN_ID="${COREVIDEO_PKG_SIGN_IDENTITY:-Developer ID Installer: John Wallace (42394L3353)}"
IDENTIFIER="us.iamfatness.corevideopro.pkg"
die() { echo "error: $*" >&2; exit 1; }

while [ $# -gt 0 ]; do
    case "$1" in
        --app)     APP="$2"; shift 2 ;;
        --version) VERSION="$2"; shift 2 ;;
        --out)     OUT="$2"; shift 2 ;;
        --sign)    SIGN_ID="$2"; shift 2 ;;
        -h|--help) sed -n '2,19p' "$0"; exit 0 ;;
        *) echo "error: unknown argument '$1'" >&2; exit 2 ;;
    esac
done
[ -n "$APP" ] && [ -n "$VERSION" ] && [ -n "$OUT" ] || die "--app, --version and --out are required"

case "$SIGN_ID" in
    *"Developer ID Installer"*) ;;
    *) die "signing identity '$SIGN_ID' is not a \"Developer ID Installer\" certificate (the Application identity cannot sign a .pkg)" ;;
esac
IDS="$(security find-identity -v)"
grep -qF "\"$SIGN_ID\"" <<<"$IDS" || die "installer identity not in keychain: $SIGN_ID"

PLIST="$APP/Contents/Info.plist"
[ -f "$PLIST" ] || die "'$APP' is not an app bundle (no Contents/Info.plist)"
APP_VER="$(/usr/libexec/PlistBuddy -c 'Print :CFBundleShortVersionString' "$PLIST")"
[ "$APP_VER" = "$VERSION" ] \
    || die "version mismatch: --version $VERSION but the app reports CFBundleShortVersionString $APP_VER"
MIN_OS="$(/usr/libexec/PlistBuddy -c 'Print :LSMinimumSystemVersion' "$PLIST" 2>/dev/null || echo 13.0)"
MIN_MAJOR="${MIN_OS%%.*}"
codesign --verify --deep --strict "$APP" || die "app failed codesign --verify --deep --strict"

W="$(mktemp -d "${TMPDIR:-/tmp}/cvp-pkg.XXXXXX")"
trap 'rm -rf "$W"' EXIT
# --component with a bundle path: pkgbuild copies it as-is. Disable relocation
# so an old copy elsewhere on disk is never "upgraded" instead of /Applications.
cat > "$W/component.plist" <<PL
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0"><array><dict>
<key>BundleHasStrictIdentifier</key><true/>
<key>BundleIsRelocatable</key><false/>
<key>BundleIsVersionChecked</key><false/>
<key>BundleOverwriteAction</key><string>upgrade</string>
<key>RootRelativeBundlePath</key><string>CoreVideo Pro.app</string>
</dict></array></plist>
PL
# Stage via ditto (preserves signature, symlinks, xattrs incl. the stapled ticket)
mkdir "$W/root"
ditto "$APP" "$W/root/CoreVideo Pro.app"
pkgbuild --root "$W/root" --component-plist "$W/component.plist" \
    --identifier "$IDENTIFIER" --version "$VERSION" \
    --install-location /Applications "$W/component.pkg" >/dev/null

# Second component: the core copy the camera verifies. Its own component with
# its own install-location, so its BOM names only that folder: a single payload
# rooted at / would also carry /Applications and /Library/Application Support
# with root:wheel 755, which differ from the system's (root:admin).
CORE_DIR="/Library/Application Support/CoreVideo Pro"
CORE_IDENTIFIER="us.iamfatness.corevideopro.core.pkg"
APP_CORE="$APP/Contents/Resources/corevideo-native"
[ -x "$APP_CORE" ] || die "app has no core at $APP_CORE"
mkdir "$W/coreroot"
ditto "$APP_CORE" "$W/coreroot/corevideo-native"
chmod 755 "$W/coreroot" "$W/coreroot/corevideo-native"
cmp -s "$APP_CORE" "$W/coreroot/corevideo-native" || die "staged /Library core differs from the app's core"
codesign --verify --strict "$W/coreroot/corevideo-native" || die "staged /Library core fails codesign --verify"
pkgbuild --root "$W/coreroot" --identifier "$CORE_IDENTIFIER" --version "$VERSION" \
    --install-location "$CORE_DIR" "$W/core.pkg" >/dev/null

# The camera extension binary must be in the payload: a pkg without it installs
# an app whose virtual camera can never activate, and nothing else would notice.
EXT_PAYLOAD="CoreVideo Pro.app/Contents/Library/SystemExtensions/us.iamfatness.corevideopro.shell.camera.systemextension/Contents/MacOS/CoreVideoProCamera"
PAYLOAD_LIST="$(pkgutil --payload-files "$W/component.pkg")" \
    || die "pkgutil --payload-files failed on $W/component.pkg"
grep -qxF "./$EXT_PAYLOAD" <<<"$PAYLOAD_LIST" \
    || die "pkg payload lacks $EXT_PAYLOAD"
# Without the /Library core the camera installs and then refuses every frame.
CORE_PAYLOAD="$(pkgutil --payload-files "$W/core.pkg")" || die "pkgutil --payload-files failed on core.pkg"
[ "$(sort <<<"$CORE_PAYLOAD" | tr '\n' ' ')" = ". ./corevideo-native " ] \
    || die "core.pkg payload is not exactly the core: $CORE_PAYLOAD"

cat > "$W/distribution.xml" <<DX
<?xml version="1.0" encoding="utf-8"?>
<installer-gui-script minSpecVersion="2">
    <title>CoreVideo Pro</title>
    <options customize="never" require-scripts="false" hostArchitectures="arm64"/>
    <domains enable_anywhere="false" enable_currentUserHome="false" enable_localSystem="true"/>
    <volume-check><allowed-os-versions><os-version min="$MIN_OS"/></allowed-os-versions></volume-check>
    <choices-outline><line choice="default"><line choice="$IDENTIFIER"/><line choice="$CORE_IDENTIFIER"/></line></choices-outline>
    <choice id="default"/>
    <choice id="$IDENTIFIER" visible="false"><pkg-ref id="$IDENTIFIER"/></choice>
    <choice id="$CORE_IDENTIFIER" visible="false"><pkg-ref id="$CORE_IDENTIFIER"/></choice>
    <pkg-ref id="$IDENTIFIER" version="$VERSION" onConclusion="none">component.pkg</pkg-ref>
    <pkg-ref id="$CORE_IDENTIFIER" version="$VERSION" onConclusion="none">core.pkg</pkg-ref>
</installer-gui-script>
DX
mkdir -p "$(dirname "$OUT")"
rm -f "$OUT"
productbuild --distribution "$W/distribution.xml" --package-path "$W" \
    --sign "$SIGN_ID" --timestamp "$OUT" >/dev/null

# Verify: signature, and the payload app still verifies (signature preserved).
SIGOUT="$(pkgutil --check-signature "$OUT")"; echo "$SIGOUT"
grep -q "Developer ID Installer" <<<"$SIGOUT" || die "pkg is not signed by a Developer ID Installer identity"
pkgutil --expand-full "$OUT" "$W/expanded"
PAYLOAD_APP="$(find "$W/expanded" -name 'CoreVideo Pro.app' -maxdepth 4 -type d | head -1)"
[ -n "$PAYLOAD_APP" ] || die "expanded pkg has no CoreVideo Pro.app"
codesign --verify --deep --strict "$PAYLOAD_APP" || die "payload app signature broken by packaging"
# The shell runs the installed core only when it and its folder are root-owned
# and writable by nobody else (CoreLocation.rootOwnedAndLocked): pin that here.
CORE_BOM="$(lsbom -p fMUG "$W/expanded/core.pkg/Bom")" || die "lsbom failed on the core component"
[ "$(awk '{print $1" "$2" "$3" "$4}' <<<"$CORE_BOM" | sort | tr '\n' ';')" \
    = ". drwxr-xr-x root wheel;./corevideo-native -rwxr-xr-x root wheel;" ] \
    || die "core component is not root:wheel 755: $CORE_BOM"
PAYLOAD_CORE="$(find "$W/expanded/core.pkg" -name corevideo-native -type f | head -1)"
[ -n "$PAYLOAD_CORE" ] || die "expanded pkg has no core component payload"
cmp -s "$PAYLOAD_CORE" "$PAYLOAD_APP/Contents/Resources/corevideo-native" \
    || die "payload /Library core differs from the payload app's core"
echo "pkg ok: $OUT (payload app signature verified, min macOS $MIN_OS, arm64)"
