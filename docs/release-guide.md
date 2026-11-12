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

## Report a problem

Include the version (for example `v1.0.0`), Windows version, CPU and GPU, the
steps, what you expected, what happened, and the approximate local time. Say
whether the problem shows in Program, Multiview, a recording or an external
output. Diagnostics > Export support bundle collects redacted logs; review a
bundle before sharing it, and never include passwords, meeting passcodes or
authorization links.

Uninstall removes the delivered application files and shortcuts while keeping
settings, recordings, downloaded media and other files created after install.
