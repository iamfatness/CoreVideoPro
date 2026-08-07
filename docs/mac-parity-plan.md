# macOS parity plan (2026-08-06)

Derived from a **visual** audit — the Windows reference screenshots in
`docs/design-reference/` compared against the real macOS UI rendered through the
`COREVIDEO_SHELL_SNAPSHOT` harness — not from matching control labels between
XAML and Swift.

## Why the earlier audit was wrong

The first parity pass grepped WinUI control labels and looked for matching Swift
symbols. It produced a plausible list that was wrong in both directions:

* it reported "Live keyer" as a missing **chroma keyer**. It is the downstream
  lower-third keyer, and the core implements no chroma keying at all.
* it reported **SRT** and **VST3** as macOS shell gaps. Both were missing CORE
  features — SRT's sender returned `nullptr` on every platform, and the plug-in
  host is Windows-only past `--scan`.
* it ranked **brand kit** as "P2 polish". Seen side by side, the entire Overlays
  tab is a stub.

A label diff cannot see depth, workflow, or information density — which is
exactly what "it doesn't feel like the Windows app" describes.

**And a visual audit has its own trap**: the two builds were not in comparable
states (Windows had live inputs, assigned sources and a meeting; macOS had
none). Empty rows look identical to missing controls. Two suspected gaps —
per-source Grade/Unassign/ISO on Sources, and the half-empty multiview canvas —
turned out to be state differences, confirmed by reading the code. **Every gap
below was verified in source, not just in pixels.**

---

## P0 — the operator cannot do the job on macOS

### 1. Overlays is a stub (~15% of the Windows tab)

macOS has ONE card: lower-third Name, Title, Position, Show — plus a sentence
telling the operator to use the Media tab for a logo bug. Windows has four
sections. Missing:

* **Live keyer**: LT in / Rebuild, source-behaviour explainer, lower-third
  position, style, **build-in / build-out ms**, motion-timing preset, and
  **Lower third / Bug / Image** graphic buttons with a configured-graphics list.
* **Brand kit**: kit name, logo text, logo asset, default-overlay behaviour, and
  Primary / Accent / Background colours with pickers.
* **Browser overlays (DSK)**: graphics URL + canvas size + Add overlay.
* **Captions**.

This is shell work: the core already accepts `set-overlay-asset`
(text/imageUri/position/title/org/keyPosition/keyer/**buildInMs**/**buildOutMs**,
clamped 50–2000ms), `set-brand-kit`
(name/logoText/brandColor/accentColor/backgroundColor/fontFamily/lowerThirdStyle/
captionStyle/defaultOverlayBehavior), `push-caption-cue` and
`set-caption-enabled`, and publishes `brandKit` in the snapshot.

Exception: **Browser overlays need core work** — the browser host is a WebView2
process, so macOS needs a WKWebView equivalent. Ship the rest without it.

### 2. Per-source microphone pairing is missing

Windows has a "Pair a microphone" dropdown on every assigned input row; macOS
has none (`audioDeviceId` appears zero times in the mac shell). This is not
cosmetic: it is how a capture card's audio is attached to its video, and
`MediaCore::isoSourceHasAudio` uses exactly that pairing to decide whether a
capture ISO carries an audio track. Without it, capture sources are silent and
their ISOs are video-only.

---

## P1 — the operator flies blind

### 3. Status-strip telemetry — MOSTLY A FALSE GAP (corrected 2026-08-07)

I claimed macOS showed none of FRAME DROPS, the LIVE timer, or the master meter
with LUFS and peak. Reading the code rather than the screenshot: the status row
**already renders** MASTER, LUFS, a meter bar, peak dBFS and a LIVE timer. They
appeared as "—" in my capture because there was no audio and no recording — the
THIRD time a state difference impersonated a missing feature in this audit.

Genuinely missing, and now fixed: **FRAME DROPS**. `recording.totalDroppedFrames`
was published by the core and parsed nowhere in the shell, so a recording could
lose frames with no sign of it in the operator's view.

NOT a gap: Windows draws separate **L / R** meter bars, but the core's
`masterMeterState()` publishes only `momentaryLufs / shortTermLufs /
integratedLufs / truePeakDbfs / windowMs` — no per-channel levels. A single
meter on macOS is correct until the core publishes stereo.

### 4. SuperSource background

Windows' Studio rail has a background picker plus "Import background media".
macOS has nothing (`SuperSource` appears zero times).

---

## P2 — core work, not shell

5. **Browser sources / DSK overlays** — needs a WKWebView host process.
6. **Chroma key** — the core implements none. `setParticipantTransform` takes an
   unnamed parameter and discards its payload; there are no key fields on the
   render-plan layer and no shader math. The `chroma-key` capability string is
   advertised (and listed as REQUIRED) while nothing implements it — either
   implement it or stop claiming it.
7. **SRT ingest** — delivery now works; ingest is a scaffold that discards
   packets.

## P3 — finish the audit

### Media — compared 2026-08-07. Mostly UI-over-nothing on Windows.

Windows shows four sections against macOS's one card, but three of the missing
pieces are Windows UI for things the CORE DOES NOT IMPLEMENT:

* **LUT preset** — the core has NO LUT support. A case-insensitive grep for
  "lut" returns 27 hits which are all the middle of "reso-LUT-ion"; a
  case-sensitive search for `LUT`/`lutPreset`/`.cube` returns nothing. Building
  a LUT picker on macOS would be a control that cannot do anything.
* **Chapter markers** — zero references anywhere in `native/src/`.
* **Cue in Preview / Still image playout buttons** — no matching core concept.

Genuinely worth considering: per-ASSET exposure/contrast/saturation. The core
does support per-ROUTE `colorGrade` (`route.get("colorGrade")` →
`layer.hasColorGrade`), so a media asset composited as a route could carry its
own grade — but Windows stores it per asset, which is a different model. Needs a
decision before it is built, not a port.

macOS's Media bin is otherwise equivalent (import, refresh, per-asset select /
still / remove) — it looked emptier only because the bin had no assets.

### Still un-compared

**Zoom, Routing, Audio, Automation, Health.** These need sources assigned and a
meeting joined, or state differences will manufacture false gaps as they did
three times in this audit. Automation is already covered separately by a
per-feature source audit (core-backed vs shell-policy vs not-supported).
Audio looked thin only because macOS had no PCM channels.

## Blocked on hardware / account

* Capture-card verification (owner's 8–10 card shows) — no cards on hand.
* Virtual camera + notarised packaging — needs an Apple Developer ID.

---

## Goal 1 — bring Overlays to parity (minus browser DSK)

Build the Live keyer controls, the Brand kit, and Captions against the commands
the core already exposes. Browser overlays are explicitly out of scope until a
macOS browser host exists, and must be shown as unavailable rather than omitted
silently.
