# CoreVideo Pro — ranked backlog

**This is the only ordered list of work.** Status lives on the linked GitHub
issue (`backlog` label). How agents must treat this file:
[`AGENTS.md`](../AGENTS.md).

Owner-approved as the single list on 2026-09-10. Re-ranked 2026-09-17 for
turnkey Zoom production that can compete with vMix / Vectar / mimoLive / Ecamm:
show survival first, then one source bus + NDI/SRT, MXL parked (#539).

`docs/FOCUS_PLAN.md`, `docs/beta-plan.md`, and
`docs/native-production-completion-plan.md` are rationale or build specs.
Do not trust their checkboxes.

## How to use this file

1. Work the **Now** sequence. Do not pick from Later because it is interesting.
2. New finding → GitHub issue the same day → row here → owner ranks.
3. Max 3 Now items in flight. A PR names one issue.
4. Move a row to **Done** only when the issue is closed by a PR or an owner
   “won’t do.”

Rank clauses (beta = 5–20 external operators on machines we cannot see):

1. Stops a show breaking on an unseen machine
2. Lets us diagnose a failure nobody watched
3. Removes a papercut during a real show
4. Required for the first external install

Sizes: XS < 1 h, S ≈ 1–3 h, M ≈ half to one day, L > one day (agent time).

---

## Strategy (2026-09-17)

Ship a Zoom room that looks finished and hands clean program + ISOs + NDI/SRT
to a real plant. The source bus (#535) is how that plant grows. MXL is how it
grows after the bus exists.

WIP still 3. Work Now[1–3]; refill from Now[4–5] then Next.

## Now — show survival (start here)

Show-survival cleared 2026-09-18: #529/#533 (ISO drops+timeline), #516 (render stall /
GPU-fence coupling), #526 (Program-buffer misses) and #518 (FADER LAW audio log) all
closed — fixed, merged and soak/load-validated (24-min 6-ISO recording: 0 dispatch
drops; 12-min multiview soak: 0 stalls). Shipped in `beta-2026-09-18-c80ee51`.

| Order | Issue | Item | Why |
|---|---|---|---|
| 1 | [#513](https://github.com/iamfatness/CoreVideoPro/issues/513) | Idle XAML 0xc000027b | Ship-blocker between shows. Editor-teardown (#548) + multiview pooling (#549) shipped — exposure reduction, NOT closure. Needs a long idle soak of the new beta + the CsWinRT/WinAppSDK framework angle. |
| 2 | [#535](https://github.com/iamfatness/CoreVideoPro/issues/535) | F1 one source bus | **Slice 0 shipped 2026-09-18; slice 1 (Zoom video, per-participant `ISource`) merged 2026-09-19 (#553)**, regressed live within 90 min ([#554](https://github.com/iamfatness/CoreVideoPro/issues/554), bus source dropped across a subscription gap → program slate / "flashing"), fixed same day (`ZoomBusRoster.h` parity rule: only remove a Zoom source absent from BOTH the decoded frames and the engine's subscription roster). **Slice 2 (capture devices, `CaptureDeviceSource`/`syncCaptureSources`) merged 2026-09-19 (#558)** — the opposite removal rule from Zoom (capture adapters re-emit their held frame every tick, so absence means gone), gated clean by `scripts/qa/zoom-gap-hold-ab.py`, the full Windows dev suite, the stub gate, `validate-multiview.mjs`, and the show drill. **Slice 3a (media frames onto the bus, parity, `layers` retained) ready on branch `codex/535-slice3-media`, unmerged, 2026-09-19** — `MediaAssetSource`/`syncMediaSources` mirror the producer at media's two existing injection points (post-roster-merge for stills, post-plan for decoded media); the request set, pause/hold, cue→Program hand-off (#449) and `preview:` poster key are deliberately untouched. Gated clean by the full Windows dev suite (1065/0), the stub gate, `validate-multiview.mjs`, the headless `validate-tiles.mjs` pixel oracle (substituting for `validate-show-engine.mjs`, which needs a running WinUI app), `zoom-gap-hold-ab.py` (luma held, no slate dip), and the show drill (60fps/100% delivery). **Slice 4a (bus health on air) ready on branch `codex/535-slice4a-health-on-air`, unmerged, 2026-09-19** — done-when #3 ("no per-kind special case for empty frames") delivered: render-plan layers carry `sourceHealth`/`dropoutPolicy`/`sourceDisplayName`, and the CPU preview/D3D11/Metal compositors share ONE resolution rule (`compositor::slateColorFor`/`blackOnStalled`) instead of each inventing `colorFromParticipantId` ("pink tile" retired everywhere). Per the owner's 2026-09-19 rulings: warming = neutral dark slate, failed/missing = dark slate + source name, stalled = the operator's per-source "On dropout" choice (hold last frame, default, or black). **Final-review fix wave, same branch, 2026-09-19 — the first cut above was too broad and is corrected here:** (R1) on-air "stalled" is **Zoom-only**, and is a **1.5s hysteresis** (`core::kOnAirStallNs`), deliberately distinct from the bus's 200ms diagnostic health in `sources[]` — capture/media/still sources NEVER read "stalled" from frame-cadence alone (a browser source, WGC screen capture, a still or a paused clip legitimately re-serves the same frameId for long stretches while healthy), and a Zoom source recovers the instant a new frame arrives. (R2) a dropout **policy is Zoom-only this slice** — `set-source-policy`/`source.dropout.set` refuse a non-`zoom:<pid>` id with a loud warning/`ControlInvokeResult.Fail` (capture liveness via `signalPresent` isn't wired to a stall decision yet, so a capture "black" policy would act on a signal that doesn't mean the same thing for that kind — that wiring is a **follow-up**, gated on `signalPresent`); the capture-row "On dropout" combo is removed from the Sources page, and preferences load prunes any persisted non-`zoom:` policy key. A source's **displayName is still accepted and stored for any id** (capture included) — only the policy is Zoom-gated, because the failed slate needs names for capture too. (R3) **black applies to the PROGRAM pass only** — preview and multiview force every layer's `dropoutPolicy` to "hold" after annotation (`holdDropoutForMonitoring`), so the operator can watch a stalled source for recovery on the monitoring surfaces even while Program has cut to black. (R4) `slateColorFor` now also treats "stalled" as the failed slate (dark + name) when there is no content frame to fall back on. (R5) the failed-slate name is now the **resolved** roster/device name (operator override, else derived), for every current Zoom participant and capture device — not just ids that already had an override or an explicit policy. Gated clean: full Windows dev suite (1080/0), `dotnet test` MediaCore.Tests (2228/0)/Control.Tests (74/0)/WinUI.Tests (1510/0), Release x64 WinUI build (0 errors), and TWO `zoom-gap-hold-ab.py` runs — `--policy hold` (default `--gap-seconds 0.45`) held luma 187.5–204.7 through the whole recording (no dip, correctly under the 1.5s window); `--policy black --gap-seconds 2.5` held luma ~199–205 for the first ~1.3s of the gap, dropped to exactly luma 16.08 for ~1.35s once the 1.5s on-air window elapsed, and recovered to ~199+ immediately after restore. **Not done in 4a:** the three old poll interfaces (`IZoomCaptureSource`/`ICaptureDevice`/`IMediaFrameSource`) still exist; that interface retirement is now **slice 4b**, per `docs/superpowers/specs/2026-09-19-source-bus-slice4-scoping.md`. **Next:** an owner ruling on Take-semantics (#449 step 1) unblocks slice 3b (drops the `layers` argument, moves media request state/hand-off/poster into the source, own spec); slice 4b then does the real interface retirement (Zoom audio onto the bus, an adapter lifecycle contract, media's slice 3b, and capture dropout policy gated on `signalPresent`). |
| 3 | [#555](https://github.com/iamfatness/CoreVideoPro/issues/555) | First Take of a fresh Zoom source renders half off-screen | Owner-reported 2026-09-19 on the slice-1 build, "new". Not reproduced headlessly (no per-layer geometry on the wire; fake engine never restarts a stream). Owner re-test on the #554 fix first; then the compositor geometry log line named in the issue. Rank clause 3 (real-show papercut) — clause 1 if it survives the re-test. |

## Next — competitive spine

| Order | Issue | Item | Why |
|---|---|---|---|
| 1 | [#538](https://github.com/iamfatness/CoreVideoPro/issues/538) | SRT send + NDI send harden first | vMix/Vectar table stakes. Demo B. |
| 2 | [#536](https://github.com/iamfatness/CoreVideoPro/issues/536) | SRT ingest decode onto the bus | After #535. |
| 3 | [#423](https://github.com/iamfatness/CoreVideoPro/issues/423) + T2 leftovers | Signing + first external install | Turnkey dies at SmartScreen. Calendar, not code. |
| 4 | [#449](https://github.com/iamfatness/CoreVideoPro/issues/449) step 1 | Hold outgoing picture on a plain cut | Needs owner ruling on Take semantics. |

## Papercuts (not competitive)

| Issue | Item |
|---|---|
| [#562](https://github.com/iamfatness/CoreVideoPro/issues/562) | Capture-device dropout policy needs adapter liveness via `signalPresent` — #535 R2 refuses a policy for any non-`zoom:` id this slice; wiring capture liveness into a real stall decision is the follow-up that lifts the restriction |
| [#551](https://github.com/iamfatness/CoreVideoPro/issues/551) | ISO writer `framesWritten`/`bytesWritten` read 0 mid-recording for async writers (status-only; files record fine) |
| [#456](https://github.com/iamfatness/CoreVideoPro/issues/456) | Media In/Out points — needs UI design; was previous Now #4 |
| [#530](https://github.com/iamfatness/CoreVideoPro/issues/530) | Control API `programPreview` vs `program-preview` |
| [#521](https://github.com/iamfatness/CoreVideoPro/issues/521) | GPU-direct leftover slices #524 #525. **2026-09-20:** HEVC on GPU-direct SHIPPED (1080p60 gate green); **AV1 is REFUSED, not shipped** - it binds the AV1 MFT and runs at the correct cadence but emits near-empty access units (~54 bytes/frame vs H.264's ~12,483 at 1080p60, ~18 kbit/s against 6 Mbps), so it refuses at start with `codec-not-deliverable` pending the payload defect ([#565](https://github.com/iamfatness/CoreVideoPro/issues/565); `node scripts/validate-gpu-encode.mjs --codec av1` is what flips it back). Refuse-never-downgrade shipped (sub-project 1 of the 2026-09-20 spec); sub-project 2 (raw fallback at real time) and sub-project 3 (recording/ISO on the seam, absorbs #525) are next and need their own specs. |
| [#517](https://github.com/iamfatness/CoreVideoPro/issues/517) | Program render budget at 16 guests + 1080p wall |
| [#519](https://github.com/iamfatness/CoreVideoPro/issues/519) | Fake RTMP `bytesSent` / `latencyMs` |
| [#509](https://github.com/iamfatness/CoreVideoPro/issues/509) | Operator stutter / 197 ms rebuild |
| [#508](https://github.com/iamfatness/CoreVideoPro/issues/508) | Multiview labels after unassign |
| [#507](https://github.com/iamfatness/CoreVideoPro/issues/507) | Debug text in operator UI |
