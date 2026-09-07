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

## SRT — delivery now shipped; ingest still a scaffold

Owner decision, same day: **both directions are required** for a pro AV
application, superseding FOCUS_PLAN §4's "pick one".

**Delivery: done and proven (#380, #381).** It rides the shared FFmpeg sender
rather than a second libsrt integration — the staged FFmpeg is built with
libsrt, and the RTMP sender already owns the hardened process pipeline (pacing,
NV12 feeding, reconnect/backoff, health). RTMP and SRT now differ only by a
protocol profile: destination name, container (FLV vs MPEG-TS), endpoint syntax
and validation. Verified end to end against a real FFmpeg SRT listener on
loopback — 5.98 MB of h264 1920x1080 received clear, 5.05 MB encrypted with a
passphrase — by `scripts/validate-srt-output.mjs`, which fails unless decodable
video actually lands in the receiver.

**Ingest: still a scaffold, and less complete than a first read suggests.**
`SrtIngestCaptureAdapter` is not "libsrt ingest that needs enabling":

- `pumpSource` receives into a 1316-byte buffer and **discards it**, incrementing
  byte/packet counters. A comment says "Decode is handled by the decoder stage";
  there is no decoder stage.
- `pollVideoFrames` therefore emits `VideoFrame`s carrying width/height and
  **no pixel payload at all**.
- So even with `COREVIDEO_WITH_SRT_INGEST` + `COREVIDEO_HAS_LIBSRT` enabled and
  libsrt linked, ingest would show nothing.

The design that matches the now-proven delivery path: spawn one FFmpeg per
source reading `srt://…` and writing fixed-size raw frames to stdout
(`-f rawvideo -pix_fmt bgra`), read them on the receiver thread, and publish
real pixels as `capture:<id>` — the same shape as the browser-source host. That
removes the libsrt build dependency entirely, matching delivery.

Passphrase-at-rest was already done: `StreamSrtPassphrase` rides the DPAPI
prefs (v4).

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
