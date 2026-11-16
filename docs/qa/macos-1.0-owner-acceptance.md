# CoreVideo Pro for macOS 1.0: owner acceptance

Everything automation on this branch could not prove. Run it on the owner's Mac with
the **notarized, signed DMG build** unless a step says otherwise. Record pass or fail
and the date next to each item. A single fail blocks the release.

Prerequisites: a camera, a microphone, a second Mac or phone to play the Zoom
attendee, an RTMP test destination, FFmpeg installed (`brew install ffmpeg`), and the
app installed to `/Applications` from the DMG (the bundle is `"/Applications/CoreVideo Pro.app"`,
with a space; quote the path in every command).
The release itself is notarized with CoreVideo Pro's own notarytool profile, `CoreVideo Pro`
(one-time: `xcrun notarytool store-credentials "CoreVideo Pro" --team-id 42394L3353`; the
OBS plugin's `CoreVideo Plugin` profile is a different product's).

## 0. Release gate: the full native test suite, realtime included

Shared CI runners exclude the `realtime` ctest label (the AVF encoder/capture timing
tests, `corevideo-native-tests-realtime`), so CI never runs them. `scripts/release-macos.sh`
is the gate that does: it runs the FULL native `ctest` on the release core build, realtime
included, before anything is packaged, and stops the release on any failure (or if no
`realtime` test is listed at all).

1. Run the release on a quiet machine (no other build, soak or drill running).
2. Pass: the script prints `core ctest: 100% tests passed out of N (realtime label: Total Tests: 1)` and the
   `manifest.json` `coreTests` entry shows `realtimeIncluded: true`. A failed or skipped
   realtime test blocks the release; do not re-run with the label excluded.

## 1. Install and Gatekeeper

1. On a Mac that has never run this build (or after deleting the app and its quarantine
   state), download the DMG, open it, drag the app to Applications, and launch it.
2. Pass: no Gatekeeper warning or "unidentified developer" dialog; `spctl -a -vv
   "/Applications/CoreVideo Pro.app"` reports `source=Notarized Developer ID`;
   `xcrun stapler validate` on the DMG succeeds.
3. Repeat from a clean state with the `.pkg` instead: double-click
   `CoreVideoPro-<version>-arm64.pkg`. Pass: no Gatekeeper warning; the installer
   completes; `CoreVideo Pro.app` is in `/Applications` and launches;
   `spctl -a -vv -t install` on the .pkg reports `source=Notarized Developer ID`,
   `pkgutil --check-signature` shows the Developer ID Installer chain, and
   `xcrun stapler validate` on the .pkg succeeds.

## 2. Permission prompts appear once and persist

1. First launch: use a camera, a microphone and a screen source.
2. Pass: one prompt each for Camera, Microphone, Screen Recording; none repeats on relaunch.
3. Install a newer build over the old one (use a rehearsal build with a bumped build number),
   relaunch, use the same sources.
4. Pass: no permission prompt reappears and the sources still work.

## 3. Quit while live (Cmd-Q)

Start a recording first, then repeat with a stream running instead.

1. Press Cmd-Q.
2. Pass: the "Stop outputs and quit?" dialog renders fully, with both buttons visible and
   unclipped. "Keep running" keeps the app and the recording going.
3. Press Cmd-Q again and choose "Stop and quit". While it says "Finishing your files",
   press Cmd-Q a second time and click around.
4. Pass: the buttons respond during the wait; the second Cmd-Q does not start a second stop or
   crash; the app quits only after the file is closed, and the recording plays back
   start to end in QuickTime. For the stream: the destination shows the stream ending.
5. Close the main window (red button) with a recording running, then press Cmd-Q.
6. Pass: the standalone quit panel appears, unclipped (reference render:
   `docs/qa/macos-quit-panel-fallback-2026-11-14.png`), and its buttons work.

## 4. Real camera ISO frame count (#984)

1. Record a Program recording plus an ISO of a real camera for 60 seconds.
2. Count source: the adapter's camera arrival counter (frames the camera delivered to the
   capture adapter, shown in Health or the core log for that device) is the expected
   count. The ISO file's frame count is
   `ffprobe -count_frames -select_streams v -show_entries stream=nb_read_frames <iso.mp4>`.
3. Pass: the ISO frame count equals the arrival counter, off by at most 1 frame. Then check
   every frame step: `ffprobe -select_streams v -show_entries packet=pts_time -of csv=p=0
   <iso.mp4>` has no repeated pts and no delta larger than 1.5x the camera frame period.
   Any duplicate or gap fails, even if the totals match.

## 5. Zoom sign-in and join (hardened runtime)

1. From the Zoom tab, sign in, then join a meeting where the signed-in account is host or co-host.
2. Pass: sign-in completes, the join succeeds, participant video appears, and a Zoom source
   can be put on Program. The Zoom engine launches from the signed bundle (no crash dialog,
   no "damaged" error).

## 6. Transitions and Fade to Black

1. Put two different sources in Preview and Program. Press Take with Fade.
2. Pass: the fade is visually smooth on Program (no steps, flashes or dropped frames).
   Take with Cut switches instantly.
3. Press Fade to Black.
4. Pass: Program fades to true black (check the Program output or a recording, not only
   the on-screen readout) and a second press restores the picture.

## 7. Keyboard

1. With the main window focused on the Studio tab: Space, Cmd-Return, Cmd-Shift-B.
2. Pass: Space and Cmd-Return Take; Cmd-Shift-B toggles Fade to Black.
3. Click into any text field (scene name, stream key) and type a space.
4. Pass: the space is typed and no Take happens.

## 8. Companion and Stream Deck

1. Point Bitfocus Companion (same Mac: `127.0.0.1`, port 8011) at the app, then repeat from
   another computer, launching the app from a terminal as
   `COREVIDEO_HTTP_LAN=1 COREVIDEO_CONTROL_TOKEN=<secret> "/Applications/CoreVideo Pro.app/Contents/MacOS/CoreVideoProShell"`.
2. From a Stream Deck button for each: Take, Fade to Black, scene select.
3. Pass: the connection shows green, and each button acts on the Mac within a second
   and the button feedback matches app state. Without a token, the LAN connection is refused.

## 9. RTMP stream from a Finder launch

1. Quit the app, then launch it by double-clicking it in Finder (not from a terminal), with
   FFmpeg installed through Homebrew.
2. Start a stream to a real RTMP destination for 10 minutes. (RTMP is the only network
   output in macOS 1.0; there is no SRT output or SRT/RTMP ingest on the Mac.)
3. Pass: the stream goes live and stays up; the destination shows a clean picture and sound
   with no stalls. With FFmpeg uninstalled, the app says FFmpeg is missing rather than failing silently.

## 10. 1080p60 frame delivery

Per CLAUDE.md "Frame-delivery acceptance": 60 fps is per-frame delivery, never an average.

1. On the owner's Mac, with the release core build (the one inside the DMG app, not a
   debug or dev-tree core), Program buffer at its default: `python scripts/mac-show-drill.py
   --seconds 40 --load 8`.
2. Pass: zero Program-buffer underruns, zero missed scheduled output deadlines and zero
   dropped frames in the drill's verdict, and every structural gate passes. Any one of these
   fails the tested configuration; a 60 fps mean or median never waives it. A render overrun
   absorbed by the buffer is diagnostic only.
3. Record in the release notes: Mac model and macOS version, the workload (`--load 8`,
   duration, enabled outputs), the core binary path and build, the Program buffer setting,
   and every failure or warning the drill printed, with the full output attached.

## 11. Full show rehearsal

1. Run a complete rehearsal: configure scenes, sign in to Zoom, bring a live participant,
   take between scenes, run a lower third, record, stream, fade to black, stop and quit.
   Run at least 30 minutes.
2. Pass: no crash, no stuck state, no unexplained warning; the recording is playable and
   in sync; the app quits cleanly.

## 12. Virtual camera

Roadmap M9.2. Run with the notarized `.pkg` build. Items marked "after Task 8/10" need the
camera rename (Task 8) and the 720p behaviour (Task 10), which land in the owner session; do
not run them against a build without those tasks.

1. Fresh install from the notarized `.pkg`, first enable of Virtual camera, approval prompt.
   Expected: after approval, "CoreVideo Pro Camera" shows Program in **Photo Booth**,
   **FaceTime**, **Zoom**, **Teams in Chrome**, **Google Meet in Chrome and in Safari**, and
   **OBS** (Video Capture Device).
2. Approval denied, then granted later. Expected: the flyout moves from the approval text to
   live with no relaunch.
3. Launch the app from the DMG without installing. Expected: "Move CoreVideo Pro to the
   Applications folder to use the virtual camera." and nothing is installed.
4. Sleep 2 minutes in the middle of a Zoom call, then wake. Expected: Zoom shows the slate
   during the gap and Program again within 5 s of the core resuming; never a frozen frame.
5. Quit the app mid-call. Expected: slate within 1 s. Relaunch: Program again with no toggle
   (the intent persisted).
6. Update: install build N+1 over N while Zoom is closed. Expected: if the extension changed
   (higher `extensionVersion`), the first launch replaces it silently
   (`systemextensionsctl list` shows the new version) and the camera works; if it did not
   change, nothing is submitted and `systemextensionsctl list` is unchanged.
6a. Update while Zoom uses the camera: install a build with a higher `extensionVersion` while
   Zoom has CoreVideo Pro Camera selected in a call. Expected: nothing is replaced, the flyout,
   Settings > System and Health show "An update to CoreVideo Pro Camera is ready. Quit apps
   that are using the camera (Zoom, Teams, Photo Booth...), then click Update camera.", and
   Zoom keeps working. Quit Zoom, click Update camera. Expected: the extension swaps with no
   restart, `systemextensionsctl list` shows only the new version `[activated enabled]`, and
   Photo Booth shows Program.
6b. Forced deferred case: reproduce a stuck swap (an old copy `[terminated waiting to
   uninstall on reboot]` and no device, e.g. replace while Photo Booth streams from a build
   without this fix). Expected: within 10 s of launch the status is "Restart your Mac to finish
   updating the virtual camera." in all three places; after a restart the camera works.
7. (after Task 8/10) Set Camera name to "Studio A", toggle the camera off and on. Expected:
   Photo Booth lists "Studio A". Also check the 720p behaviour Task 10 defines.
8. Turn on OH Framer and Mirror. Expected: both visible in Zoom's self-view; the Program
   recording stays clean (check the recorded file's pixels at a guide location).
9. Settings > System > Remove virtual camera (enter the password if asked). Expected: the
   device disappears from Photo Booth after relaunching it, and the next enable asks for
   approval again.
10. After the first signed launch of the camera build, the stream key and Zoom sign-in are
    still present (Review Focus 5).
11. Per-frame delivery: a live 1080p60 Program with framer and mirror on for 60 s. Expected:
    core `framesPublished` delta >= 3599, `mac publication gaps 0`, `skipped 0`, and the e2e
    probe reports consumer-side fps >= 59 at 60 fps. If the probe cannot measure the worst
    interval, report it as unverified (CLAUDE.md frame-delivery rule); a mean never waives this.

## Known not on the Mac in 1.0

Graphics designer/operate, Comms, social review, Tiles and the multiview inspector.
See CHANGELOG.md. These are not acceptance items.
