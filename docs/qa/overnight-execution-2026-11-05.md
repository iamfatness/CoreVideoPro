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
