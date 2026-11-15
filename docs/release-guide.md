# CoreVideo Pro

CoreVideo Pro is a Zoom-first live production console for Windows: bring Zoom
participants, cameras, screens and media into one switcher, cue talent over
Zoom talkback, and record, stream or send the show out over NDI, SRT and a
virtual camera.

## Install

1. Download the Setup installer and its SHA-256 checksum from the
   [CoreVideo Pro releases page](https://github.com/iamfatness/CoreVideoPro/releases).
2. Run Setup. It adds desktop and Start menu shortcuts and a Windows uninstall
   entry. This release is not yet code-signed, so Windows may show an
   unknown-publisher warning.
3. Setup installs or updates Microsoft's signed Visual C++ x64 runtime when
   needed, and asks for administrator approval for the virtual camera and for
   crash-dump settings. First launch downloads and verifies the pinned FFmpeg
   media runtime, so internet access is required for that step.

Use a Windows 10 or later x64 PC with a Direct3D 11 GPU and current graphics
drivers. Installing a newer version retires older CoreVideo Pro installs while
keeping your settings, recordings and downloaded media.

## Install on a Mac

1. Download `CoreVideoPro-<version>-arm64.dmg` and its `.sha256` checksum from
   the releases page. The Mac build runs on Apple Silicon (M-series) Macs with
   macOS 13 or later; there is no Intel build.
2. Open the DMG and drag **CoreVideo Pro** onto **Applications**. The app is
   signed with a Developer ID and notarized by Apple, so it opens without a
   Gatekeeper override.
3. On first use, macOS asks for camera and microphone permission (and screen
   recording permission for a screen source). If a prompt was dismissed, allow
   it in System Settings > Privacy & Security.
4. **Streaming (RTMP output) needs FFmpeg**, which is not
   bundled. RTMP is the only network output in the Mac 1.0 build; SRT output and
   SRT/RTMP ingest are Windows-only for now. Install it with [Homebrew](https://brew.sh): `brew install ffmpeg`.
   CoreVideo Pro finds Homebrew (`/opt/homebrew/bin`, `/usr/local/bin`) and
   MacPorts (`/opt/local/bin`) installs automatically, including when the app
   is opened from Finder or the Dock. Local recording does not need FFmpeg.

### First-run permissions (Mac)

CoreVideo Pro asks once for each of these, then remembers the answer, including
after you install an update:

- **Camera** for camera sources.
- **Microphone** for audio inputs.
- **Screen Recording** for screen and window sources (macOS may ask you to quit
  and reopen the app after you allow it).

If you dismissed a prompt, turn it back on under System Settings > Privacy &
Security. Sign in with Zoom from the Zoom tab before your first meeting (see
below). Control from Bitfocus Companion is covered in its own section further
down.

## Before your first show

- Sign in with Zoom. Sign-in uses the CoreVideo broker at
  `corevideo.iamfatness.us`.
- Rehearse with a disposable show: select a source in Preview, press Take, and
  confirm Program shows it.
- Make a short local recording and play it back to confirm picture, sound and
  sync before going live.
- Comms (Zoom talkback) needs CoreVideo Pro's Zoom account to be host or
  co-host, and talent on the Zoom desktop or mobile app (the web client cannot
  receive talkback). Turn on **Private comms** if the room must never hear you.

## Control CoreVideo Pro from Bitfocus Companion

CoreVideo Pro serves a local control API on **port 8011** (HTTP and WebSocket) on
Windows and on the Mac. It starts with the app and listens on `127.0.0.1` only.
In `companion-module-corevideopro`, add a connection and set:

- **Host**: `127.0.0.1` when Companion runs on the same computer, otherwise the
  Mac's address (for example `studio.local` or its LAN IP).
- **HTTP port**: `8011` (or the value of `COREVIDEO_HTTP_PORT` if you set one).
- **Bearer token**: leave blank on the same computer. For a LAN connection, set
  the same value you launched CoreVideo Pro with as `COREVIDEO_CONTROL_TOKEN`.

To let another computer reach a Mac, launch CoreVideo Pro with
`COREVIDEO_HTTP_LAN=1` and a non-empty `COREVIDEO_CONTROL_TOKEN`
from a terminal (for example
`COREVIDEO_HTTP_LAN=1 COREVIDEO_CONTROL_TOKEN=<secret> "/Applications/CoreVideo Pro.app/Contents/MacOS/CoreVideoProShell"`).
Without a token the app refuses to listen beyond loopback. The connection shows
green once Companion's WebSocket opens, and its action list is read from the
app's `/manifest`.

On the Mac, the control API provides Take, Fade to Black, Record, Stream and
scene select (`transport.take`, `transport.ftb.set`/`toggle`, `transport.record.*`,
`transport.stream.*`, `scene.select`, `scene.list`). If the port is already in
use, the app shows a warning and keeps running without remote control.

## Cut a Mac release (maintainers)

One-time setup on the release Mac, so notarization can run unattended:

```
xcrun notarytool store-credentials "CoreVideo Pro" --team-id 42394L3353
```

The profile is named `CoreVideo Pro` (the OBS plugin has its own, separate profile).
To use a different name, set `COREVIDEO_NOTARY_PROFILE` when running the script.

Then, from a clean tree on the release branch:

```
scripts/release-macos.sh --skip-notarize   # rehearsal: dist/macos/<v>-UNNOTARIZED/
scripts/release-macos.sh                   # release:   dist/macos/<v>/
```

Both runs first build the release core and run the FULL native `ctest` on it,
including the `realtime` label (the AVF encoder/capture timing tests). CI excludes
that label on shared runners, so this is the only place those tests run; any
failure, or a core that lists no realtime test, stops the release before anything
is packaged. Run it on a quiet machine. The result is recorded in `manifest.json`
(`coreTests`). The app is assembled inside the script's temp work dir, so your dev
bundle at `mac-shell/.build/CoreVideo Pro.app` is left alone.

The rehearsal builds and signs everything but submits nothing to Apple, and its
output is named `-UNNOTARIZED` so it cannot be mistaken for a release. The real run
notarizes and staples the app and the DMG, verifies them, and only then moves the
result into `dist/macos/<v>/` (DMG, `.sha256` and `manifest.json`); a previous
good release is kept if any step fails. It needs the Zoom macOS SDK
(`ZOOM_SDK_DIR`) and the Developer ID identity in the login keychain. Before
publishing, work through `docs/qa/macos-1.0-owner-acceptance.md`.

## Report a problem

Include the version (for example `v1.0.0`), Windows version, CPU and GPU, the
steps, what you expected, what happened, and the approximate local time. Say
whether the problem shows in Program, Multiview, a recording or an external
output. Diagnostics > Export support bundle collects redacted logs; review a
bundle before sharing it, and never include passwords, meeting passcodes or
authorization links.

Uninstall removes the delivered application files and shortcuts while keeping
settings, recordings, downloaded media and other files created after install.
