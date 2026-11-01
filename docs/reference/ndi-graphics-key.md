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
  converts and publishes the latest frame. A slow reader skips frames, never the render.
- **Cadence.** The key is produced on every Program render tick while active and
  sent on the output worker's Program-paced video tick (edge-triggered on a new
  Program frame), declared at the stream profile's fps like the Program NDI source.
  Each send carries the newest key the tap published (latest-wins).
- **Sender.** `createNdiGraphicsKeyOutputSender` is the same runtime-probed libNDI
  adapter as Program in a `GraphicsKey` role: its own `AsyncOutputSender` writer and
  supervisor, `senderId` `ndi:graphics`, destination `ndi-graphics`, capability
  `ndi-graphics-output`. **No audio**: a receiver takes Program audio from the Program
  source. `NDIlib_initialize`/`destroy` are reference-counted across the two senders.

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
`CoreVideo Pro Program Graphics`), or the operator's own name. It uses the Program
source's NDI group. It rides with NDI output: it is sent only while NDI is enabled
and streaming is on, and can be toggled live without touching the Program source.

- **UI.** Production settings, NDI section: "NDI graphics key" and "Graphics key
  name" (blank = the default above). The NDI summary line names the key or says the
  core build cannot carry it.
- **Persistence.** `StreamNdiGraphicsKeyEnabled` and `StreamNdiGraphicsName` in the
  output preferences. Additive fields, no version bump: older files load with the key off.
- **Control API.** `transport.stream.ndi.graphicsKey.set` with `on` (bool) and
  optional `name` (omitted keeps the current name, empty restores the default):

  ```json
  {"action":"transport.stream.ndi.graphicsKey.set","args":[true,"Studio Key"]}
  ```

  OSC: `/cvp/transport/stream/ndi/graphicsKey/set`. Control state:
  `ndiGraphicsKeyOn`, `ndiGraphicsKeyName` (selection, not delivery). The sender's
  real state is `outputSenderSession` in `GET /snapshot`; its `runtimeDetail` ends
  `video=graphics-key-bgra-straight-alpha`.
- **Validation.** Starting a stream with the key selected on a core whose profile
  lacks `ndi-graphics-output` is refused with a message naming it.

## Isolation

Like Program NDI, the key sender is in-process libNDI with no `interrupt()`
(`destinationIsolationTraits("ndi-graphics")`, see output-supervisor.md). It has its
own writer thread, so a wedged key send cannot stall the Program NDI send.

## Tests and what is not verified

- Native: `GraphicsKeyPixels.*` (layer rule, unpremultiply endpoints and mid-alpha,
  pitched rows) and
  `D3D11Compositor.GraphicsKeyCarriesOnlyGraphicsWithStraightAlphaAndLeavesProgramUnchanged`
  (a real GPU composite: the key has the graphic, zero alpha and zero colour where
  only Program video existed, Program pixels byte-identical with and without the key,
  and nothing published after the flag drops). No test creates a live NDI sender.
- Shell: `NdiGraphicsKeyOutputTests` (action registered and handled, binding,
  preference round trip and old-file default, name rule, capability gate).
- **Not verified live:** a receiver (vMix, OBS, a switcher) discovering
  `... Graphics` and keying it over its own picture; edge quality of the
  unpremultiplied DirectWrite text on a real keyer; the extra GPU and readback cost
  at 4K60 with the key on. The tap reads back at Program size, so a 4K Program makes
  a ~33 MB/frame readback.
- Metal/macOS: `takeGraphicsKeyBgraShared` is not implemented there; the sender
  reports `key-pixels-missing` and stays `starting`.
