# NDI graphics key (fill+key output)

Titler spec Phase 1 ("NDI fill+key", `docs/titler-service-spec.md`). A second NDI
source that carries **only the on-air graphics**, composited over transparency, so a
downstream switcher, vMix or OBS can key CoreVideo's graphics over its own picture.
The Program output, including the Program NDI source, is unchanged.

## What is on the key

Exactly the native graphics instances the On Air strip shows on Program: the plan
layers `GraphicsInstances::append` produced, i.e. layers with a `graphicsInstanceId`
and overlay content (`graphicsKeyLayerSelected` in
`native/src/modules/GraphicsKeyPixels.h`, the one predicate the compositor, the
tests and this page share). Video layers never reach the key. Legacy scene overlays
without a native instance (old lower-third assets, captions) stay Program-only until
they are migrated to instances.

## How it is produced

- **Plan flag.** MediaCore sets `CompositorRenderPlan::graphicsKeyReadback` only while
  the `ndi-graphics` destination is in the output list. With it off, the compositor
  frees the key target and its reader thread on the next tick, so an idle key costs
  nothing.
- **GPU pass** (`D3D11Compositor::renderGraphicsKeyPass`). After the Program pass,
  the same resolved layers are drawn a second time, graphics instances only, into a
  fourth BGRA render target at **Program resolution**, cleared to `0,0,0,0`, with the
  same pipeline state the Program pass used. The overlay rasters are content-cached,
  so the extra cost is the graphics draws plus one GPU-to-GPU copy. The pass records
  no admission or "draw-submitted" facts; the Program pass already did.
- **Readback off the render thread** (`D3DGraphicsKeyTap.h`). Same shape as the
  virtual-camera tap: the render thread does a non-blocking keyed-mutex
  `CopyResource`; a second D3D device on its own thread copies to staging, maps,
  converts and publishes. A slow reader skips frames, never the render. Output
  buffers come from a recycled pool of at most 10 (no per-frame 8 or 33 MB
  allocation); rows that are entirely transparent or entirely opaque are a
  `memset`/`memcpy`, and only mixed rows pay the per-pixel divide.
- **Frame-matched, not latest-wins.** Each readback is tagged with the compositor
  frame number that produced it and kept in a 6-entry ring (`GraphicsKeyRing`).
  The output worker asks for the key of the Program frame it is about to send. With
  a Program buffer (2 or 3 frames) that is the DELAYED frame, so the graphics on the
  key are the ones that were on air with that Program picture; they never lead it
  by the buffer depth. Without a buffer the readback can trail the render by about
  a frame; the newest key produced at or before the Program frame is used.
- **Cadence.** The key is produced on every Program render tick while active and
  sent on the output worker's Program-paced video tick (edge-triggered on a new
  Program frame), declared at the stream profile's fps like the Program NDI source.
- **Stale key goes transparent, loudly.** No fresh readback for 250 ms (a rig that
  cannot build, staging Map failures, a stalled reader) and the output worker sends
  a fully transparent key; the sender reports status `warning`, `lastResultCode`
  `key-stale`, with a warning. It never re-sends the last graphic: a downstream
  keyer would otherwise hold a cleared lower third on air with health reading live.
- **Failure backoff.** A key target or reader rig that cannot be built is retried
  for the same size only every 5 s and logged once per failing size
  (`GraphicsKeyRetryLatch`); it never rebuilds a D3D device on the render thread
  every tick. A new size is tried at once.
- **Sender.** `createNdiGraphicsKeyOutputSender` is the same runtime-probed libNDI
  adapter as Program in a `GraphicsKey` role: its own `AsyncOutputSender` writer and
  supervisor, `senderId` `ndi:graphics`, destination `ndi-graphics`, capability
  `ndi-graphics-output`. **No audio**: a receiver takes Program audio from the Program
  source. `NDIlib_initialize`/`destroy` are reference-counted across the two senders.
  The sender re-resolves its name on every sync, so a rejected name clears as soon
  as it is fixed. Only a compositor that produces the key (`supportsGraphicsKey`,
  D3D11 today) gets the sender; on Metal and stub builds `ndi-graphics-output` reads
  `omitted` (`compositor-has-no-graphics-key`).
- **Program NDI never flaps.** Toggling the key changes only the destination list;
  each sender reacts to its own id, so the Program NDI writer sees `ndi` in every
  sync and is never interrupted or recovered
  (`AsyncOutputSender.TogglingTheGraphicsKeyNeverInterruptsProgramNdi`).
  `encoder->start(destinations)` in `start-program-output` drives only the MF
  recording encoder, not the network senders.

## Alpha convention

The compositor blends graphics "over" a transparent target, so the target holds
**premultiplied** colour. NDI's `BGRA` FourCC carries key+fill in one stream with
**straight** (non-premultiplied) alpha, and a receiver multiplies by alpha itself.
The tap therefore divides colour by alpha (`unpremultiplyBgra`, rounded and
clamped). Alpha 255 passes through exactly; alpha 0 is published as `0,0,0,0` so no
colour noise reaches a receiver that ignores alpha. The sender sends the buffer
unchanged with `NDIlib_FourCC_type_BGRA`, stride `width * 4`.

## Naming and settings

The key advertises `<NDI program name> Graphics` (for example
`CoreVideo Pro Program Graphics`), or the operator's own name. The default trims the
program part so the whole name fits NDI's 64 characters. A name over 64 characters,
with NDI's reserved characters (`< > : " / \ | ? *`), or equal to the Program
source's name is refused. It uses the Program source's NDI group. It rides with NDI
output: it is sent only while NDI is enabled and streaming is on, and can be toggled
live without touching the Program source.

**A key that cannot be sent is dropped on its own.** Missing core capability, NDI
off or an invalid name removes only `ndi-graphics` from the destination list and the
NDI summary says why; the key is never part of stream validation, so it can never
stop RTMP, SRT or the Program NDI source.

- **UI.** Production settings, NDI section: "NDI graphics key" and "Graphics key
  name" (blank = the default above). The NDI summary line names the key or says the
  core build cannot carry it.
- **Persistence.** `StreamNdiGraphicsKeyEnabled` and `StreamNdiGraphicsName` in the
  output preferences. Additive fields, no version bump: older files load with the key off.
- **Control API.** `transport.stream.ndi.graphicsKey.set` with `on` (bool) and
  optional `name` (omitted keeps the current name, empty restores the default).
  The whole request is validated first and returns an error, changing nothing, when
  NDI output is off, the core lacks `ndi-graphics-output`, or the name is invalid.
  Toggle and name are applied as one edit with one save and one output sync:

  ```json
  {"action":"transport.stream.ndi.graphicsKey.set","args":[true,"Studio Key"]}
  ```

  OSC: `/cvp/transport/stream/ndi/graphicsKey/set`. Control state:
  `ndiGraphicsKeyOn`, `ndiGraphicsKeyName` (selection, not delivery). The sender's
  real state is `outputSenderSession` in `GET /snapshot`; its `runtimeDetail` ends
  `video=graphics-key-bgra-straight-alpha`.
- **UI toggle on a core without the capability** reverts only the key toggle and
  sets the output status to say why; the stream is untouched.

## Isolation

Like Program NDI, the key sender is in-process libNDI with no `interrupt()`
(`destinationIsolationTraits("ndi-graphics")`, see output-supervisor.md). It has its
own writer thread, so a wedged key send cannot stall the Program NDI send.

## Cost and cadence limit

CPU time to unpremultiply one key frame on this dev box (Release build, one core,
`GraphicsKeyPixels.UnpremultiplyCostIsMeasuredForTypicalAndWorstFrames`, printed,
not a gate):

| Program size | Typical lower third (band over ~20 % of rows) | Worst case (every row mixed alpha) |
|---|---|---|
| 1920x1080 | 1.3-1.4 ms | 7.4-7.5 ms |
| 3840x2160 | 6.3-6.6 ms | 29.4-29.5 ms |

So the reader keeps up with 60 fps at 1080p in every case and at 4K for ordinary
lower thirds. A 4K full-frame semi-transparent graphic tops out near 34 key
frames/s on the reader thread: the reader skips frames (the ring then serves the
newest key at or before each Program frame), Program is never slowed. Not
measured: the staging `Map` + copy of the 8 MB (1080p) or 33 MB (4K) readback,
and GPU time of the second graphics draw.

## Tests and what is not verified

- Native, each run as its own exact `--gtest_filter`:
  - `GraphicsKeyPixels.*` (9): layer rule; unpremultiply endpoints, mid-alpha,
    pitched rows and the all-0/all-255 row fast paths; the ring picks the key of
    the delayed Program frame; warming, then stale when no key arrives or keys stop;
    a stale key is stamped transparent and reported `key-stale`; the retry latch;
    the cost measurement above.
  - `D3D11Compositor.GraphicsKey*` (4), real GPU composites over a grey Program
    video. For every pixel, Program equals the key keyed over the video
    (`Program = keyColour * a + video * (1 - a)`, within 4 levels), which only
    holds for a straight-alpha key carrying no Program pixels. Measured on the
    `compact-solid` band: 4,032 partial-alpha pixels (peak alpha 253), 0
    mismatches. The static case also checks Program is byte-identical with and
    without the key, the band colour is not the video's, and the rig is released
    (`graphicsKeyActive() == false`) once the flag drops. With no graphic on air
    the key is all zeros while Program shows video. Mid-animation (building in at
    0.5) and cross-fade (layer opacity 0.5) carry partial alpha (peak 126) with 0
    mismatches. A rig that cannot build is attempted once per 5 s and logged once
    per size.
  - `StubCompositor.DoesNotDeclareGraphicsKeySupport`,
    `AsyncOutputSender.TogglingTheGraphicsKeyNeverInterruptsProgramNdi`,
    `NdiSourceName.SenderNameResolutionNeverLatchesAnEarlierRejection`.
  - No test creates a live NDI sender.
- Shell: `NdiGraphicsKeyOutputTests` (15 cases): action registered, handled and
  bound; preference round trip and old-file default; name default, length,
  reserved characters and collision; refusal for NDI off or missing capability with
  nothing applied; toggle and name applied as one edit; a key that cannot be sent is
  dropped alone and never fails stream validation.
- **Not verified live:** a receiver (vMix, OBS, a switcher) discovering
  `... Graphics` and keying it over its own picture; edge quality of the
  unpremultiplied DirectWrite text on a real keyer; the transparent-on-stale
  behaviour against a real receiver; GPU and readback cost at 4K60 in the running app.
- Metal/macOS and stub builds: no key is produced and `ndi-graphics-output` reads
  `omitted`.
