# CoreVideo Pro alpha tester guide

This is an experimental Windows x64 build for rehearsals and feedback. Recording,
A/V timing, and output reliability are still under investigation. It has not
passed an every-frame 60 fps physical-output acceptance test. Keep an independent
recording for anything you cannot repeat.

## Start

1. Download the alpha ZIP and its SHA-256 checksum from the
   [CoreVideo Pro GitHub releases page](https://github.com/iamfatness/CoreVideoPro/releases).
2. Extract the entire ZIP into a new writable folder. Keep its DLLs, Assets,
   notices, and other subfolders together. Do not run inside the ZIP or merge it
   over an older installation.
3. Launch `StartCoreVideo.cmd`. On first launch, it downloads approximately 89 MB
   of a pinned FFmpeg media runtime directly from its upstream GitHub release and
   verifies the archive checksum. Internet access is required for this first step;
   keep the console open until it finishes. It installs only inside the app folder
   and does not change your system PATH. The package includes .NET and Windows
   App Runtime and the Visual C++ runtime. This alpha is unsigned, so Windows may show an unknown-publisher
   warning. Report any startup error with its exact message.
4. Start with a disposable show and a short local recording. Confirm that you can
   play the completed file with audible sound before trying a meeting.

Use a Windows x64 PC with a working Direct3D 11 GPU and current graphics drivers.
Camera/microphone privacy permissions and any optional output drivers are managed
by Windows. A bundled virtual-camera DLL alone does not establish that a camera
device has been installed. For virtual-camera testing, use the included
`Register-VirtualCamera.cmd`; use `Unregister-VirtualCamera.cmd` to remove its
registration afterward. Follow any Windows administrator prompt from those
scripts. Select the registered CoreVideo camera in your receiving application.

## Zoom sign-in

Use your own Zoom account and a test meeting whose participants consent to testing
and recording. Sign-in uses the CoreVideo broker at `corevideo.iamfatness.us` and
returns to the app. The release owner must confirm that your account is eligible
for this alpha's Zoom application; the presence of a sign-in button does not
establish public availability. If authorization is denied or the return-to-app
step fails, report that step and the error text. Never send passwords, authorization
links, access tokens, or meeting passcodes in a bug report.

## Useful checks

- Select a source in Preview, press Take, and confirm Program uses that source.
- Enable a lower third, change the Program source, and check that the name follows
  the person actually on Program.
- Record 10–20 seconds of a flash/clap or speech. Stop and wait for finalization,
  then play the saved file and note missing beginnings/endings, freezes, or lip-sync.
- In production settings, Program buffering defaults to 3 frames; 2 frames is also
  available. The selection is saved for the **next app launch**. Restart the whole
  app to apply it; toggling the engine does not apply a pending selection.

## Report a problem

Send the package version/checksum, Windows version, GPU model, buffer depth,
steps to reproduce, expected result, actual result, and approximate local time.
Mention other GPU-heavy apps running during the test. State whether the issue is
visible in Program, in a saved file, or at an external destination.

Keep reports private if they contain participant names or recordings. Review any
diagnostic bundle before sharing. App preferences and authentication data live
under `%LOCALAPPDATA%\CoreVideoPro`; do not send that whole folder. Send a short
consented sample only when requested. Closing and deleting the extracted package
does not remove these per-user settings or your recordings.
