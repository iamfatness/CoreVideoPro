# CoreVideo Pro beta tester guide

This Windows x64 beta is for rehearsals and structured feedback. It passed a
30-minute live Zoom soak with a three-frame Program buffer: 108,014 Program
frames advanced at 59.993 measured fps with zero deadline misses, underruns,
overflows, GPU-not-ready events, or sequence gaps. Keep an independent recording
for anything you cannot repeat; a finite soak cannot cover every meeting and PC.

## Start

1. Download the beta installer and its SHA-256 checksum from the
   [CoreVideo Pro GitHub releases page](https://github.com/iamfatness/CoreVideoPro/releases).
2. Run Setup. The installer adds desktop and Start menu shortcuts and a Windows
   uninstall entry. This beta is unsigned, so Windows may display an unknown
   publisher warning.
3. Setup installs or updates Microsoft's signed Visual C++ x64 runtime when
   needed. First app launch downloads and verifies the pinned FFmpeg media
   runtime; internet access is required for that step.
4. Start with a disposable show and a short local recording. Play the completed
   file and confirm picture, sound, and synchronization before joining a meeting.

Use a Windows x64 PC with a Direct3D 11 GPU and current graphics drivers. Camera
and microphone permissions are managed by Windows. Virtual camera registration
is optional and may request administrator approval.

## Zoom sign-in

Use a test meeting whose participants consent to testing and recording. Sign-in
uses the CoreVideo broker at `corevideo.iamfatness.us`. Never include passwords,
authorization links, access tokens, or meeting passcodes in a bug report.

## Useful checks

- Select a source in Preview, press Take, and confirm Program uses that source.
- Enable a lower third, change Program, and confirm the name follows the person
  actually on Program.
- Exercise Tiles with participants joining, leaving, disabling video, and
  returning.
- Record 10–20 seconds of a flash/clap or speech and check the saved file for
  freezes, flashes, missing ends, and lip sync.
- Program buffering defaults to three frames. A two-frame option is available
  and applies after restarting the app.
- Production logging is the default. Turn on Health > Extra logs only while
  reproducing a problem, then turn it off before continuing the show.

## Report a problem

Include the beta ID and checksum, Windows version, CPU and GPU, buffer depth,
steps, expected result, actual result, and approximate local time. State whether
the issue appears in Program, Multiview, a recording, or an external output.
Review diagnostic bundles before sharing them and keep participant media private.

Uninstall removes delivered application files and shortcuts while preserving
settings, recordings, downloaded media, and other files created after install.
