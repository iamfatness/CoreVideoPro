# Source bus slice 4 — scoping (why it is not the next mechanical slice)

**Status 2026-09-19:** slices 0–3a are on `main` (test pattern, Zoom video, capture, media/stills onto the bus). Issue #535 "done when" #1/#2/#4/#5 are met for VIDEO. #3 — "compositor / ISO / encode consume only that bus, no per-kind special case for empty frames" — is what slice 4 was to deliver by deleting `IZoomCaptureSource` / `ICaptureDevice` / `IMediaFrameSource`. This note records why that deletion is not yet possible, what it decomposes into, and which decisions are the owner's.

## What slices 1–3a actually moved

Each slice moved **where a frame is published to the render gather** (into per-key `ISource`s on the `SourceBus`), not **where it is produced**. Every producer still runs behind its old interface:

| Interface | Still does | Consumers today |
|---|---|---|
| `IZoomCaptureSource` | the no-engine synthetic slate (`RealZoomCaptureSource` → `SyntheticZoomCaptureSource`) **and Zoom AUDIO** (`pollZoomAudioUnlocked` → `modules_.zoom->pollAudioFrames()` every audio tick, pre-`coreMutex`) | `MediaCore.cpp` (2 sites), 6 test files that inject PCM/ISO/attribution fixtures through it (25 uses in `MediaCoreCommandTest` alone) |
| `ICaptureDevice` | device lifecycle — `enumerate / selectInput / connect(+outputSourceId) / disconnect / setAudioSyncOffset / configureSrtIngestSources` — plus the per-tick frame poll and SRT transport audio | 8 adapters (WinUI SHM bridge, UVC, WGC, SRT ingest, hardware, AVF + SCK on macOS, stub), MediaCore device commands, 2 test fixtures |
| `IMediaFrameSource` | the request-driven media player: `pollMediaFrames(layers, t)` (request set = plan layers, pause/hold, cue→Program hand-off, `preview:` poster), `pollMediaAudioFrames`, `syncMediaClock`, `prefetchMediaVideo` | `OwnedMediaFrameSource`, `MediaFoundationMediaFrameSourceAdapter`, MediaCore (video after the plan, audio in the audio gather), 5 test files |

So "delete the three interfaces" is three different projects, each with a prerequisite that is not a bus change:

1. **`IMediaFrameSource`** → needs **slice 3b** (media request state set at command time, hand-off inside the source), which is the same decision as the open Take-semantics ruling (#449 step 1). Owner ruling required. Then media AUDIO must also leave `pollMediaAudioFrames` (clap-gated).
2. **`IZoomCaptureSource`** → needs **Zoom audio onto the bus** (`SourceTick.audio` exists and is unused). The audio gather has its own architecture — pre-lock polling (`pollZoomAudioUnlocked`, the 8.5 ms lock-wait fix), `requiresSteadyFeedPriming`, the 20 ms program clock, per-participant ISO stems — and every change there is gated by the A/V clap test. Design doc + clap gate required; not a parity wrap.
3. **`ICaptureDevice`** → needs an **adapter lifecycle contract** (enumerate/connect/select/audio-offset/SRT config) that `ISource` does not carry and the spec did not design (§2 deliberately scoped `ISource` to delivery). Either `ISource` grows a lifecycle face or a sibling `IDevice` contract is added; either way it touches 8 adapters across two platforms.
4. **Compositor per-kind empty-frame fallback** (done-when #3 proper) — **DONE IN SLICE 4A, 2026-09-19.** Today (pre-4a) a layer with no content frame renders `colorFromParticipantId` (the "pink tile") and logs for capture/media keys only. Collapsing this to one bus-health path means the compositor must see `SourceHealth` (warming / stalled / failed) per layer and render *something* for each — **what a warming or stalled source looks like on air is an operator-facing design decision** (blank? last frame held? a labelled slate? per the OBS/vMix model, usually black or held frame, never pink). Owner UX ruling required; also the place where "probe-only devices read warming instead of a slate" (spec §5 slice 2) finally lands.

   **The two owner rulings that unblocked this, recorded 2026-09-19 (see `docs/superpowers/plans/2026-09-19-source-bus-slice4a-health-on-air.md`):**
   - **On-air look, per health state:** *warming* = a neutral dark slate (`kWarmingSlateRgba`); *failed / missing* = a dark slate with the source's name (`kFailedSlateRgba` + a small name label on D3D11); pink (`colorFromParticipantId`) is retired from every layer-resolution path for every layer kind.
   - **Stalled sources are the OPERATOR's per-source choice, not a fixed rule:** *stalled* (had frames, none for 200 ms+) renders **hold last frame** (default) or **black**, chosen per source on the Sources page ("On dropout"), persisted, and shipped to the core as `set-source-policy`.

   Delivered as one bus-health resolution rule shared by the CPU preview, D3D11 and Metal (colour-only) compositors — `compositor::slateColorFor` / `compositor::blackOnStalled` in `CompositorLayout.h`. **Not done in 4a:** the three old poll interfaces (`IZoomCaptureSource`/`ICaptureDevice`/`IMediaFrameSource`) still exist — items 1–3 above are unaffected and remain the real interface-retirement work, now tracked as **slice 4b**.

## Recommended order

1. **Owner: rule on #449 step 1** (hold outgoing picture on a plain cut / go-live semantics). Unblocks 3b (media) and defines the hand-off; nothing else in #535 should move media before it.
2. **Owner: rule on the on-air look of a warming / stalled / failed source** (one sentence per state). Unblocks item 4, which is small once decided (the compositor already has `retainedProgram`/held-texture machinery and the loud placeholder logging). **DONE 2026-09-19 — see item 4 above; delivered as slice 4a.**
3. **Design doc: audio onto the bus** (Zoom first, then media, then capture transport) — clap-gated, its own spec; then retire `IZoomCaptureSource`.
4. **Design doc: adapter lifecycle contract**; migrate the 8 adapters; retire `ICaptureDevice`.
5. Retire `IMediaFrameSource` after 3b + media audio.

## What is unblocked right now (small, no rulings needed)

- The deferred minors from slices 2–3a: descriptor width/height refreshed on a dims change (media + capture); `setLatest` rvalue overload (one struct copy per frame per tick); per-kind skip counters in `MediaBusRoster`; stage attribution for the bus syncs (charged to "merge" today).
- Sources page: a sliding-window `deliveredFps` (2 s) instead of the since-first-frame average; a row hint for the honest "@ 0fps" state.
- #555 diagnostic: the compositor geometry log line on change (only if the half-off-screen report recurs).

None of these advance done-when #3; they are hygiene while the two owner rulings are pending.
