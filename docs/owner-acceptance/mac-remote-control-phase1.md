# Owner acceptance: macOS remote control, Phase 1 (RC1-RC4)

Plan: `docs/superpowers/plans/2026-11-16-mac-remote-control-phase1.md`. Tracking: #1217.
Everything below needs the **installed, signed** build from this branch, launched the way an operator
launches it. The agent that implemented the plan did not run any of it (the owner's app was live on
127.0.0.1:8011 and nothing could be sent to it). Record results inline.

## 1. Conformance and setup drill (loopback)

1. `node scripts/control-conformance.mjs --host mac --base http://127.0.0.1:8011` — expect every line
   `PASS`. Result:
2. With a capture device connected and no meeting:
   `python3 scripts/control-setup-drill.py --base http://127.0.0.1:8011 --slot 1 --source capture:<id> --scene <sceneId> --record-seconds 5`
   then `ffprobe -show_streams -count_frames <recording>` shows a non-empty video stream. Result:

## 2. Owner check 1 (RC2): Companion on the same Mac

Join a meeting, assign two guests, set a fade transition, Take. Note which Companion buttons lit. Result:

## 3. Owner check 2 (RC3): audio from Companion

Monitor on, volume 0.3, mastering on; hear it. Result:

Lower-third pixels: record, `graphics.lowerThird.set true`, wait 3 s, `false`, wait 2 s, stop; then
`ffmpeg -i <file> -vf "crop=iw:ih*0.2:0:ih*0.75,signalstats,metadata=print:key=lavfi.signalstats.YAVG" -f null -`
— the band's mean luma while keyed differs from the second before by more than 8. Result:

## 4. Owner check 3 (RC4, loopback OSC)

TouchOSC or `oscsend` on the same Mac sends `/cvp/transport/take` to `127.0.0.1:8010`; `/cvp/state/canTake`
comes back to the sender's port. Result:

## 5. Owner check 5 (T21): LAN OSC and LAN HTTP

1. Install the `.pkg`; do not run the app from the build folder.
2. ```
   launchctl setenv COREVIDEO_OSC_LAN 1
   launchctl setenv COREVIDEO_OSC_TRUSTED_NETWORK 1
   launchctl setenv COREVIDEO_HTTP_LAN 1
   launchctl setenv COREVIDEO_CONTROL_TOKEN <a fresh token>
   open -a "CoreVideo Pro"
   ```
   (Launching the binary from Terminal makes Terminal the responsible app and proves nothing about the prompt.)
3. From a second machine or tablet on the same network, send `/cvp/transport/take` to `<mac-ip>:8010`. Record:
   (a) any Local Network or firewall prompt and its text; (b) whether the Take happened; (c) whether
   `/cvp/state/canTake` arrived back on the tablet. Result:
4. From the second machine: `curl -H "Authorization: Bearer <token>" http://<mac-ip>:8011/state` gives 200;
   without the header gives 401. Quit, `launchctl setenv COREVIDEO_CONTROL_TOKEN ""`, relaunch: Settings
   > Remote control shows `HTTP Off — LAN HTTP/WS control requires a non-empty COREVIDEO_CONTROL_TOKEN…`
   and port 8011 refuses connections. Result:
5. `launchctl unsetenv` all four variables, relaunch: the row reads `Loopback only`. Result:

**Conditional change** — only if step 3 shows an unexplained Local Network prompt or feedback silently
fails to arrive: add `NSLocalNetworkUsageDescription` = "CoreVideo Pro sends remote-control feedback to OSC
devices on your network when LAN control is enabled." to the Info.plist block in
`scripts/make-macos-shell-bundle.sh` (next to `NSCameraUsageDescription`), rebuild, re-sign, reinstall and
repeat steps 2-5. If no prompt appears and feedback arrives, make no plist change and say so here.
