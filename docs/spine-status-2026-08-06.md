# Competitive spine status — audit of 2026-08-06

Audit of the FOCUS_PLAN §4 spine features against their stated definition of
done, done on the Windows owner rig. Written because "NDI and SRT exist in the
tree" turned out to mean very different things for each.

## Headline

**A UI thumbnail was being shipped as the program by three separate output
paths.** `ProgramFrame::preview` is a 320x180 raster meant for the operator UI.
The program recorder, and NDI, both treated it as "the program". RTMP had the
same bug and was fixed earlier the same day by someone else. The virtual camera
was always correct.

| Output | Was it sending the real program? | Now |
|---|---|---|
| Virtual camera | yes (full-res NV12 tap) | unchanged |
| RTMP | **no** — fixed earlier 2026-08-06 by the RTMP audio/no-video fix | correct |
| Program recording | **no** — 320x180 in the corner of a black frame, since at least 2026-07-13 | fixed (#372, #373) at 1080p |
| NDI | **no** — every receiver saw a postage stamp | fixed (#375) |

The common cause is that `preview` is easy to reach and looks like program
pixels. Anything consuming program video should take `programNv12` (Windows) or
`programFullBgra` (macOS) and treat `preview` as a last resort worth warning
about.

## NDI — partially closed

Definition of done from FOCUS_PLAN §4:

- [x] Discoverable name pattern (`NdiSourceName.h`, canonicalised + tested)
- [x] Start/stop does not leak the sender (`destroySender()` on drop and in the destructor)
- [x] Sends the actual program — **fixed 2026-08-06 (#375)**, was the 320x180 preview
- [ ] Health: **connections and framesSent exist; observed fps and dropped-frame counters do not**
- [ ] "Appears in Studio Monitor / OBS NDI" — **not verified**: this rig has no NDI receiver installed, so the pixel fix is code-verified only
- [ ] Firewall / discovery note in the quickstart

The pixel fix needed NV12 -> UYVY (`NdiPixelConvert.h`), a pure byte shuffle with
a 4:2:0 -> 4:2:2 chroma resample. It is unit-tested for exact packing and carries
a 1080p cost bound so nobody reintroduces per-pixel maths on the output worker.

## SRT — not shipped in either direction

- `createSrtOutputSender()` is an **empty stub**: it returns `nullptr` on both
  sides of its `#if`. There is no SRT output.
- `SrtIngestCaptureAdapter` is **real** (352 lines of libsrt) behind
  `COREVIDEO_WITH_SRT_INGEST` + `COREVIDEO_HAS_LIBSRT`, with a synthetic
  fallback that says so in its status text.
- Both build flags are **OFF** in the current build, so neither is compiled.
- Passphrase-at-rest is already done: `StreamSrtPassphrase` rides the DPAPI
  prefs (v4).

FOCUS_PLAN §4 requires **one primary direction done well** and explicitly says
not to half-build both. Ingest is much further along. **This needs an owner
decision before any implementation** — it is a product call about who the
feature serves (contribution/CDN out, versus remote cameras in), not a technical
one.

## Recording — one gap left open deliberately

The NV12 program tap is a pinned 1080p scale-blit, so program recording is
routed through it only on an exact 1080p match. A non-1080p recording still
falls back to the preview and is still wrong; as of #377 it now says so in
`recording.warning` instead of failing silently. Closing it properly needs a
program-sized readback (~8MB/frame at 4K), which is a perf decision for the
owner.

## The process lesson

Every recording validator asserted that streams existed and that container
start/duration aligned. **None ever looked at a pixel**, which is exactly why a
black frame with a thumbnail in the corner passed every gate for months.
`validate-record-audio.mjs` now asserts median luma and frame coverage (#374),
verified to fail the 2026-07-13 artifact and pass the fixed one.
