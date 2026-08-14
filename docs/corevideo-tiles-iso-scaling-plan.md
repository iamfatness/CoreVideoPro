# CoreVideo Tiles + ISO Scaling

**Status:** implementation contract
**Priority:** launch-defining, after the Zoom ISO audio path is truthful and metering/routing is proven

## Product decisions

1. **CoreVideo Tiles is a first-class program/preview source.** It is not a switch that
   restores decorative borders on every scene route. The previous no-route-borders rule
   remains intact; a Tiles composite owns its own visual treatment.
2. **Dynamic membership and ordering come from the show engine.** Reuse
   `show-engine/src/galleryDirector.ts`, the live-slot model, speaker recency, and tally
   derivation. Do not create a second gallery authority in WinUI or the native core.
3. **Rendering stays on the GPU.** "CPU fallback" applies to ISO *encoding*, not gallery
   composition. A large gallery must not fall back to CPU compositing.
4. **ISO encoder placement is automatic by default and observable per track.** Program,
   stream, and virtual-output reservations are accounted for before ISO writers are
   assigned. Hardware capacity is consumed first; overflow is deliberately assigned to
   the supported OS software path.
5. **No hidden mid-file encoder substitution.** Placement is decided before a writer
   opens. If a hardware writer fails during startup, retry on the next supported tier.
   A failure after media has been written rolls a new segment and is recorded in the
   manifest rather than silently changing the encoder inside one file.

## Tiles v1 operator surface

The scene editor gets an **Add CoreVideo Tiles** action and a Tiles inspector. The first
release exposes the controls already proven in the OBS source:

| Group | Controls | Initial/default behavior |
|---|---|---|
| Membership | Auto/manual, max tiles, exclude list, manual order | Auto, up to 9 |
| Geometry | 16:9, 4:3, 5:4, 1:1, 3:4, 9:16, custom ratio | 16:9 |
| Spacing | Gutter and outer margin, percent of canvas height | `100 / 135%` (8 px at 1080p) |
| Border | Width, color, square/rounded, corner radius | Width 0; off |
| Glow | Size, color, intensity, softness | Size 0; off |
| Motion | Animated reflow, duration | Off for migrated scenes; on in new Tiles presets at 350 ms |
| Framing | Fill/crop plus per-slot left/right crop | Center crop |
| Background | Color or another visual source | Neutral color |

Visual-source backgrounds use the shared native media decoder. Media Foundation is
the fast path; production MOV codecs unsupported by Windows (including ProRes HQ/4444)
fall back to the staged FFmpeg runtime off the render thread. Tiles and SuperSource
must reference the same media asset/playback contract—neither feature gets a separate
decoder or requires a lower-quality proxy.

Background media loops continuously and retains its last good frame while a decoder
restarts at the loop boundary. Stingers and ordinary media routes remain one-shot by
default. Preview and Program therefore share one explicit `mediaAssetLoop` decision;
file extension or decoder choice must never silently change playback semantics.

Keep the OBS-compatible bounds unless usability testing gives a reason to narrow them:
border width 0–64 px, corner radius 0–128 px, glow size 0–256 px, glow intensity and
softness 0–100%, animation 100–1000 ms, and spacing 0–10%.

### Ownership and data flow

```text
Zoom roster + speaker events
          |
          v
show-engine GalleryDirector ---- operator overrides / exclusions
          |
          v
Tiles assignment + TilesStyle ---- persisted scene definition
          |
          v
native render plan ---- D3D11 / Metal tile mask, border, glow, crop
          |
          +---- Preview
          +---- Program -> recording / stream / virtual camera
```

The shared scene schema should add a composite node rather than expanding every route:

```text
TilesComposite {
  id, membershipMode, maxTiles, orderedSourceIds, excludedSourceIds,
  tileAspectPreset, customAspect, gutterPercent, marginPercent,
  borderWidthPx, borderColor, borderShape, cornerRadiusPx,
  glowSizePx, glowColor, glowIntensityPercent, glowSoftnessPercent,
  animate, animationDurationMs, background,
  slotCrops[]
}
```

The native render plan expands this node to ordinary participant layers plus a
Tiles-only effect descriptor. Generic `SourceRoute.BorderStyle` remains forced to `none`
in program/preview. This preserves the July 31 regression guard while allowing the
intentional on-air graphic.

## ISO encoder scaling

### Windows placement order

CoreVideo Pro does not ship GPL software encoders. The Windows CPU fallback is the
Media Foundation H.264 software transform, not `libx264`.

At recording arm, build one immutable plan for the session:

1. Reserve hardware sessions for active program recording and NVENC stream outputs.
2. Assign ISO tracks to a supported hardware H.264 transform while measured/declared
   capacity remains.
3. Assign overflow tracks to the Media Foundation software H.264 transform.
4. If a hardware writer fails before its first successful sample, reopen that track on
   the software transform and record the fallback reason.
5. If a writer fails after samples have been committed, finalize the current segment,
   open a continuation segment on the next tier, and add both to the manifest.

The existing `MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS` flag must become a per-writer
choice. "GPU" writers enable hardware transforms; "CPU" writers explicitly disable
them. A label without this behavioral difference is not an implementation.

### Required telemetry

Every program/ISO stream status and the recording manifest must include:

```text
requestedEncoderMode: auto | hardware | cpu
selectedEncoderPath: nvenc | videotoolbox | media-foundation-hardware | media-foundation-software
selectionReason: preferred | hardware-capacity-exhausted | hardware-unavailable | startup-failure
segmentIndex
videoFramesWritten
videoFramesDropped
encoderBacklogMs
```

The transport readout should summarize the plan before recording (for example,
`Program + 8 ISOs · 7 GPU / 1 CPU`) and show a loud warning if predicted CPU load or
disk bandwidth is unsafe. Per-track encoder path and fallback reason belong in the ISO
status/details view and support bundle.

### macOS

VideoToolbox remains the only supported encoder tier until a separately licensed and
benchmarked CPU path exists. The planner must report `capacity unavailable` instead of
pretending that Windows' Media Foundation fallback exists on macOS.

## Implementation slices

1. **Contract and parity tests:** Tiles style schema, serialization, defaults, and a
   pure ISO placement policy with hardware-budget boundary tests.
2. **Tiles scene plumbing:** Add Tiles to the scene model/editor and drive membership
   from `GalleryDirector`; prove join, leave, exclusion, ordering, and tally behavior.
3. **GPU effects:** Rounded mask, border, and outer-glow passes in D3D11, followed by
   Metal parity. Effects must degrade independently so a missing glow never blanks the
   wall.
4. **Motion:** Port the immediate set-change/spring animator behavior; joins fade in,
   departures and reflow stay bounded, and disabling animation is an exact bypass.
5. **Encoder allocator:** Make Media Foundation hardware preference per writer, reserve
   program/stream capacity, assign overflow to software, and publish actual decisions.
6. **Failure recovery and manifest:** Startup demotion, segmented runtime failover,
   dropped-frame/backlog reporting, and support-bundle capture.
7. **Live acceptance:** Real Zoom roster churn with 1–9 tiles; mixed GPU/CPU ISO run;
   forced hardware exhaustion; 30-minute A/V sync and file-playability soak.

## Launch gates

- Zoom meeting mix and each participant ISO stem meter independently and reach only the
  buses selected by the operator.
- Tiles renders the same membership, geometry, border, shape, glow, and animation in
  preview and program; no generic route border can leak on-air.
- A capacity-boundary test proves hardware sessions are never oversubscribed.
- A forced hardware-start failure produces a playable CPU-encoded ISO and an explicit
  fallback reason.
- Mixed hardware/software ISOs remain synchronized to program through a 30-minute soak.
- The operator can see the encoder plan before pressing Record and the actual path for
  every track afterward.
