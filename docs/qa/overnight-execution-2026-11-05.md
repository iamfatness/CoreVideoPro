# Overnight execution evidence — November 5–6, 2026

Source candidate: c9ab4c55d6a97de5b7516173e72de7da7a6e2579. Evidence root: D:/CoreVideo-QA/overnight-20261105. Local package only; no beta publication or installer update occurred. Previous published beta remains rollback.

## Implemented and merged

| Issue | PR | Demonstrated result |
|---|---|---|
| #1143 | #1161 | Controlled ISO FIFO overflow and completed-late-write placement; decoded identity accounting, zero unexplained drops in the corrected late fixture. No queue expansion. |
| #1158 | #1162 | Deterministic retired-but-charged cache lease and renewed renderer demand; no production cache change. |
| #1157 | #1163 | Validate preview metadata before preparing drafts; distinguish transient busy from protocol error; private pixels and on-air isolation tested. |
| #1137 | #1164 | One consistent completion sample in retirement fixture; controlled old failure, immutable evidence and eventual idle behavior; hosted TSAN passes. |

All four PRs passed their required hosted checks before merge. Final integrated Release passed 2,074 native cases across four CTest gates and 5,256 managed cases. Clean self-contained package passed runtime verification and compiled Studio (8), Graphics (49), and operator settings (33) checks. Zip SHA256: 24ccf8a89145ae3806720af367cdf782e47e16ffbc8dfff311b167ec98acee6a.

## Live evidence and remaining acceptance

The populated authorized test meeting had eight real remote video sources. Captions could not be enabled; Auto performed a native-confirmed scene change without observed duplicate remote identities; manual scene cue paused Auto and the confirmed manual Take remained authoritative. This does not cover scripted remote speaking/share/camera-off scenarios or subjective directing quality.

Live failure #1166: explicit Hide returns success for a playlist no-op while a legacy lower third stays visible. Reproduced with Auto both active and stopped. Neither Auto nor Graphics is accepted as fully repaired from this run.

Only virtual webcam devices were discovered; the attempted NDI virtual camera was unavailable through the shell control path. No physical hotplug acceptance. Tiles short-stall/video-off/departure behavior has deterministic suite coverage but no controlled live participant interruption in this session. Owner-at-keyboard Graphics usability remains unverified.

The first output attempt failed after five-minute warmup: eight selected/present remote sources but seven ISO writers. All eight feeds eventually reached 1920x1080, without creating the missing eighth writer. Recording and stream finalized and settings were restored. No 30-minute output acceptance is claimed. #1168 tracks readiness before recording selection, with original artifacts retained.

The registered camera belongs to an older installed build; its DLL hash differs from this candidate. Camera source files did not change between that installed source and the candidate, but this does not prove binary identity. No elevation/registration was performed. Actual serving-module evidence is required for acceptance.

## Rehearsal prerequisites and reconciliation

#1165 / PR #1167 removes request and arbitrary response text from persisted drill errors while retaining action/status and safe unknown-action classification. Synthetic credential tests and 58 offline harness cases pass. No real secret was observed leaking. #1168 / draft PR #1169 prewarms all requested remote video before Record and retains strict post-start geometry/writer/delivery checks. Live verification remains pending.

#970 closed against merged #1060 and the passing compiled search/filter probe. #551 closed against merged #1027 plus actual live nonzero per-writer frames/bytes for all seven running ISO writers. #618 remains open: real private Zoom SDK hosted-CI acceptance has an explicit outstanding provisioning dependency.

## Recovery

Before rehearsal, six profile files were backed up outside the evidence/repository. After the initial app rehearsal, all six were restored and hash-verified. Every later output trial retains its own preferences/roster backups; final six-file restoration must occur after all owned app/core processes exit. Do not restore while the app can overwrite files on close.

Second attempt, outputs-30min-prewarmed, also failed: all eight feeds were actual 1920x1080 for 30 seconds before Record, but native isoSourceIds and writers still contained seven. The warmup-only hypothesis is disproved as a sufficient fix. Source investigation found a stale missing-source cache after the API replaces an unavailable saved participant with a present one of the same kind; deterministic proof follows in #1168.

Both failed trials finalized safely. All six original configuration files were restored and hash-verified after the second trial. The bounded first batch concluded at 23:48 Eastern. As authorized, the four-hour follow-on starts then and ends at 03:48 Eastern; its only ranked queue and allocation are in BACKLOG. No full output pass or RC readiness is claimed.

## Four-hour follow-on implementation and rehearsal

The conditional batch began at 23:48 Eastern and is bounded to 03:48. Its implementation fixes are #1168 (stale same-kind source availability), #1166 (lower-third active authority and Auto restart suppression), and #1165 (credential-safe harness failures, merged as #1167 / 76583bf9).

The 15e1aa3b package reproduced the original saved unavailable slot and now generated all eight native ISO targets and eight writing ISO streams. The previously omitted writer reached 1,298 frames at observation. The subsequent full-workload warmup still failed: actual remote video fell to 1280x720 while subscriptions requested 1080p. #1171 retains the geometry series and failed warmup. No missing writer, source-resolution failure or output-delivery failure is silently conflated.

The first lower-third fix removed the native legacy key, but restarting Auto exposed a second bypass through the automation-owned graphic row. A focused visibility policy now honors manual suppression for that row while preserving explicitly enabled user graphics. Two regression tests fail against the old visibility calculation and pass after the fix. Final populated-meeting acceptance on 84813fa1 passes all 46 checks: visible native legacy key with Guest rows present, accepted Hide and completed key-out, ten seconds under active Auto, Auto restart with another ten seconds hidden, idempotent Hide and captions off. Manual cue/Take separately passes four checks and stays authoritative for ten seconds. Evidence: lower-third-final-live.json and manual-take-final.json.

The final local package source is 84813fa1bf7e1f760e4f0bc27864b38f2000626c. Its zip SHA256 is cbf7ff291f6b26ac5963308ff0b17b479130b0efa2822868aee549a748458368. The shell DLL SHA256 is 57c760bdd9ba2c195e02b960aa78e325aa1e304d2d88bd2f83142089f3c28c37. Native and show-engine sources are unchanged from c9ab4c55, and the same qualified native executable is retained (SHA256 96464c7bbb2d7f9ffd5ba4247ec4e58143e67b5899b66a9bd5002190fc03105b). Fresh self-contained package verification, 1,974 WinUI tests, and compiled Studio 8 / Graphics 50 / settings 33 checks pass. No local builds overlap output measurement.

A separate 60-second Auto scene-change trial on 15e1aa3b did not cut despite native recommendations; the exact hold cause was not captured. A later 84813fa1 rehearsal did change interview to panel, but this does not erase the failed trial or establish the prescribed host/guest/share matrix. #1146 remains open. #919 reproduced its already-known caught participant-list teardown callback during a compiled probe; no unhandled crash occurred.

The additional issue review captured 130 open issue bodies. Seven Graphics feature/spec issues were checked against merged PR evidence: #1096, #1098, #1103, #1106, #1110 and #1113 have implementation evidence but outstanding installed/provider acceptance; #1139 remains deferred intake. They were not closed merely because code exists. The dependency review is retained in additional-issue-review.md under the evidence root; BACKLOG remains the sole ranked queue.

Implementation delivery is tracked in PR #1169 and PR #1170; the linked issues remain the canonical merge/status records. The local output run explicitly excludes Zoom ISOs, physical capture and the registered OS camera.

## Final local output evidence and recovery

The local 1080p60 motion-clip run measured 00:31:02.424–01:01:03.179 Eastern on November 6 (1,800.75 seconds; 1,777 telemetry samples). Program and SRT stayed hardware-encoded/healthy with source progress throughout. After clean finalization, the whole Program file decoded 113,041 consecutive identities and the SRT file decoded 112,992, with zero missing, repeated, reordered or invalid identities in either file. Both decoders completed without errors. The supplementary Program GPU deadline check passes.

**Official output verdict: NOT JUDGED.** The Program and stream rows are **UNBOUNDED** because the skipped OS camera supplies the common bounded interval in this harness. Whole-file pixel continuity is useful evidence but does not establish that common interval, real-time delivery latency, audio or full-workload acceptance. No judge or gate was weakened. Artifacts: outputs-30min-local-final/summary.txt, verdict.json, measurement.json and judges/program.json / stream.json. The earlier eight-feed warmups remain failed and retained.

Machine: Intel Core i7-14700K, NVIDIA RTX 4090 (driver 32.0.16.2101), Windows 11 Home build 26200, 31.7 GiB visible RAM. This single-machine reduced workload is not fleet qualification.

All owned app/core/Zoom processes exited. The temporary clip was removed. At 01:01:37 Eastern, all six original profile files were restored and matched their original SHA256 values: production-output-preferences.json and its .bak, recent-meetings.json, show-input-roster.json, sources-workspace.json and studio-layout.json. Evidence: final-profile-restore.json. No installer, OS camera registration, public stream or published beta was changed.

RC acceptance remains open: actual eight-feed 1080p stability (#1171), matching registered-camera identity and the common full-output interval, the broader Auto directing matrix (#1146), physical capture hotplug and owner Graphics usability. The three bounded follow-on fixes have concrete regression/live evidence; these remaining requirements are not claimed fixed by that work.
