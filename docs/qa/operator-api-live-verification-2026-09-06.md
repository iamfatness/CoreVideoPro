# Live API verification — 2026-09-06

The user authorized automatic tests, spikes and soaks in the current test meeting. Authorization is recorded in CLAUDE.md and local Codex memory. Meeting credentials are not stored in this report or the harnesses.

## Reproduction

The old running executable was `artifacts/native/win-unpacked/CoreVideoPro.WinUI.exe`, started at 08:42. Neither repair build had been loaded. HTTP `/state` showed null Program and Preview IDs and disabled Take. POST `transport.take` nevertheless returned `ok: true`, with no state change. This directly reproduced the reported no-op.

## Additional fixes discovered by execution

- Take now returns an invocation-specific result. Disabled, concurrent, offline, failed and exhausted-busy operations return failure instead of unconditional API success.
- `/state` exposes native-observed Program/Preview IDs, lower-third phase/visibility and Program frame count. The native wire parser and mapper retain Preview telemetry; testing only direct snapshot deserialization had missed that mapping boundary.
- `automation.magic` exercises the existing one-shot Magic Scene command through the API.
- A real Zoom join completed about 1.1 seconds after the native 45-second deadline. A bounded five-second observation window recognizes that late success without issuing another join or retrying authentication failures. Joining with the user's full link succeeded immediately in the final build.

## Final running-build evidence

Executable: `artifacts/live-verified/win-unpacked/CoreVideoPro.WinUI.exe`. Packaged WinUI, MediaCore and Control assemblies matched the final build. The existing desktop shortcut now targets this executable.

- **90 cycles passed in 203 seconds**, checking scene cues, Take promotion and bus swap against native scene IDs, native lower-third build-in/off completion, Magic Scene preview cue, manual automation override, and valid scene state.
- Native Program frame count advanced from **1,067 to 13,755** during that pass.
- Separate automatic-Take test passed after allowing the configured hold time: native Program reached the recommendation; manual lower-third-off stayed off while automation remained enabled; manual Take paused automation and held its scene.
- No matching unhandled/fail-fast, preview-sync-failure, failed-Take or lower-third-sync-failure messages were found in the final running session's inspected launch log. The process remained alive and joined with capture enabled.
- Prior repair build also completed a 36-cycle shell/API pass. Final acceptance uses the stricter native-observed pass above.

Automated suites: **814 WinUI**, **492 MediaCore bridge**, **54 Control API** tests passed. Native C++ source was unchanged in this follow-up.

## Reproduce in an authorized test meeting

Run from the repository root with the app joined and capture enabled:

```powershell
node scripts/qa/operator-api-stress.cjs
node scripts/qa/operator-auto-take.cjs
```

Both scripts mutate the active show. They default to the local API on port 8011 and write sanitized JSON evidence under `artifacts/live-operator-qa/`. That folder contains `native-soak-results.json` and `automatic-take-results.json` from this run.

This is a roughly three-minute API stress pass plus an automatic-Take scenario, not a 30–60-minute soak or visual/pixel-level certification. It verifies actual native state and continued frame production; it does not claim every UI gesture, visual layout or output destination was tested.
