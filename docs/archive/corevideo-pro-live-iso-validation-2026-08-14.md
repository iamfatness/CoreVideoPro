# CoreVideo Pro live ISO validation — 2026-08-14

## Result

**The repaired eight-source live recording and abrupt media-core recovery gates passed. Production launch remains blocked on the full-duration soak and installer/upgrade gates.**

The first live run reproduced the launch-blocking failure: Program finalized as a playable H.264/AAC MP4, but all eight ISO files finalized at zero bytes. Diagnostics showed only one video frame accepted by each ISO writer and 170,910 encoder-queue video drops.

The encoder worker used absolute Program priority. With continuous Program audio/video queued, ISO work could not make forward progress after its first frame. The audio tick also re-enqueued unchanged Program and ISO frames before downstream mux dedup, flooding the queue and inflating drop telemetry.

The repair adds producer-side held-frame dedup, weighted Program/ISO scheduling fairness, truthful on-disk byte telemetry, and explicit MP4 finalization failure reporting.

The follow-up recovery repair also preserves the successful Zoom join intent across a media-core crash, owns the Zoom helper in a Windows kill-on-close job, retries Zoom initialization after its short SDK teardown lockout, and resumes the recording/media spine only after Zoom has rejoined.

## Repaired live run

- Meeting: eight synthetic Zoom participants plus the CoreVideo Producer client.
- Recording folder: `artifacts/native/win-unpacked/Recordings/CoreVideo Pro/corevideo-recording-20260814-152346`
- Recording duration: approximately 88 seconds.
- Program: 1920x1080 H.264 + 48 kHz stereo AAC, 5,162 decoded video frames, 163.1 MiB, 87.95 seconds.
- Eight ISOs: each 640x360 H.264 + 48 kHz stereo AAC, 2,320–3,391 decoded video frames, 7.2–99.3 MiB, 86.53–86.85 seconds.
- Encoder drops: 174 of 27,357 submitted/written frames in the post-run bundle (about 0.64%), versus 170,910 in the failing run.
- Native render loop remained approximately 60 fps; recent two-second windows were normally zero drops with isolated one-frame misses.
- Program and three speaking ISO stems had distinct decoded-audio SHA-256 hashes. The five non-speaking participants shared the expected digital-silence hash.
- Speaking-stem levels: Sarah -30.5 dB mean / -0.7 dB max; Susan -34.4 / -5.6; John -41.9 / -10.7. Silent ISO stems measured -91 dB. Program measured -22.1 / -1.3.
- Support bundle: `C:/Users/walla/AppData/Local/CoreVideoPro/support-bundles/support-20260814192711.zip`.

## Abrupt-exit recovery run

- Control path: local HTTP API (`zoom.join`, `transport.engine.set`, and `transport.record.set`), with OBS closed.
- Forced termination: `corevideo-native.exe` was killed while Zoom, the engine, Program recording, and eight ISO recordings were live.
- The old Zoom helper exited with the killed core. The replacement core launched a new helper and replayed the cached join.
- Zoom's first replacement `InitSDK` attempt returned code 14 during teardown. The supervisor waited 2.5 seconds and the second attempt returned code 0.
- The app returned to `Zoom Live` with `recording=true` and `engineOn=true` without operator input.
- Recovery status finalized as `Media core and Zoom recovered`; no transient raw-media state was misclassified as an operator pause.
- Program peak telemetry resumed immediately after rejoin, including measured samples between approximately -18 and -6 dBFS.
- Recovered artifact folder: `artifacts/native/win-unpacked/Recordings/CoreVideo Pro/corevideo-recording-20260814-190117`.
- Recovered Program plus all eight ISOs finalized as playable H.264/AAC MP4, 48 kHz stereo, approximately 47 seconds each.
- Recovered ISO isolation scan: Susan -32.8/-3.9 dB mean/max, Elena -34.2/-4.8 dB, Michelle -31.9/-5.6 dB; the five non-speaking stems were digital silence at -91 dB.

## API meter evidence

`GET /state` now exposes live `audioSources` entries with source ID, participant name, level, peak dBFS, RMS dBFS, mute state, and native status. It also exposes Program true peak/loudness plus the audio validation summary. This lets soak automation assert ISO meter behavior without screen capture.

## Remaining issues found

1. Active fragmented MP4 files can remain at zero/stale visible length until finalization even though the sink is accepting samples. The finalized files are correct, but live disk-rate/bytes observability should use a sink counter or explicit flush contract rather than filesystem length alone.
2. The recording stream summary reports Program `audioSamples=0`, while the proof section correctly reports 4,166,400 samples and 4,340 packets. Consolidate these two telemetry paths.
3. Participant display name `Elena Kovač` is preserved correctly in UTF-8 state/log reads. The ASCII-safe recording slug remains `Elena-Kova` by design.
4. Post-run audio health is globally warning because the optional local Chat Mic endpoint is silent, despite valid Zoom Program/ISO audio. Optional local capture should not obscure successful meeting-audio validation.
5. Some UI Zoom transport samples reached roughly 300–665 ms during the loaded run even though the native render loop stayed at 60 fps. This needs long-soak trend validation.
6. The 10-minute clean run showed Program video about 10 seconds longer than Program audio. The shorter recovery recording was within roughly one second. Long-soak timestamp/drift analysis remains a launch gate.

## Still required before launch

- 60-minute eight-ISO Zoom soak with scene edits, Preview/Take, participant churn, screen share, and mute/unmute.
- Repeat abrupt-exit recovery later in the 60-minute soak and verify the same automatic Zoom/audio recovery under sustained memory and encoder pressure.
- Release package install/upgrade/uninstall validation after the long live-media gates pass.
