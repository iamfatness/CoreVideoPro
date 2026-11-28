# Graphics: colour system, lower-third builder and style packs

November 26, 2026. Research and proposed product contract for Windows and macOS.
Not a claim of shipped functionality. Only [BACKLOG](../BACKLOG.md) sets the order.
It extends the [Volt graphics spec](volt-graphics-spec.md), the
[titler spec](../superpowers/specs/titler-service-spec.md) and the Phase 2 Designer
design (`../superpowers/specs/2026-11-01-graphics-designer-phase2-design.md`). It also
sits beside the uncommitted November 5 draft `graphics-authoring-presenter-spec.md` (#1139),
which covers multi-select, style copy, letter spacing and colour sampling. Where the
two overlap, this document defers to #1139 for selection and copy, and owns colour,
packs and the quick builder.

Source baseline: `origin/main` 0b5f04d6.

## 1. Owner ask

> We also need a way to set all the colors and have color pickers and an easy way to
> build some great looking lower thirds and it would be awesome to have some prebuilt
> ones that have some real style and feel for News, Sports, Worship, etc that aren't
> just these flat graphics that we have today.

That ask breaks into three outcomes, and each must work on Windows and Mac:

1. **One colour system.** The operator sets show colours once, every graphic follows them,
   and every colour choice in the app uses the same picker.
2. **A two-minute lower third.** The operator picks a style, types the names, adjusts
   colours and goes to air, without opening the Designer.
3. **Style packs with depth.** These are prebuilt looks with layered plates, angled shapes,
   gradients, shadows, real type hierarchy and staggered motion, for News, Sports, Worship,
   Corporate, Esports, Talk/Podcast and Minimal.

## 2. How it works today

### 2.1 Pipeline (shared by both platforms)

```
Shell (WinUI / SwiftUI)          Shared C++ core (native/src/graphics)          Per-platform raster
Operate | Design                 GraphicsTemplate JSON (schema 1)               Windows: D2D + DirectWrite on a
 ├ Builder (5 steps)    ──────▶  fields · brand.* tokens · layers   ─resolve─▶   software WIC target, uploaded to D3D11
 ├ Designer (layers)             Layout::visit → GraphicsPrimitive[]            Mac: Core Graphics + Core Text,
 └ Cues & automation             GraphicsMotion (atlas slices per unit)          uploaded to Metal
                                 GraphicsInstances → Program / Preview / NDI key
```

- **Templates are data, not code.** `GraphicsTemplate.cpp:636-702` parses a 1920×1080 document
  with up to 32 fields, 64 layers and depth 8, in 256 KiB. A property is a literal,
  `{"field":id}` or `{"token":"brand.*"}` (`:213-231`). There are 10 brand tokens (`:133`).
- **Both platforms read the same show file** (schema 3). The Mac preserves unknown keys
  (`GraphicsDocument.swift:7`). A pack defined as template JSON works on both with no
  shell port, and that is the main design lever in this document.
- **Drawing is the only per-platform step:** `CompositorOverlayRaster.cpp:298-412` (D2D) and
  `CoreTextOverlayRaster.mm` (Core Text twin, 848 lines). Every new visual primitive must
  land in both in the same change, with a parity fixture. Section 7 covers the parity harness.
- **Verbs:** Cue / Show / Hide (`graphics.toPreview` / `graphics.playIn` / `graphics.playOut`),
  `GraphicsPlayoutActions.cs:16-20`.

### 2.2 What the renderer can draw

| Area | Today (both platforms) | Missing for "premium" |
|---|---|---|
| Shapes | rect (one corner radius), ellipse, diagonal line, group (flow/gap/padding/auto width), text, image, crawl, table | **polygon/skew/chevron plates**, per-corner radius, cut corners, rotation |
| Fill | solid; **2-stop linear gradient, horizontal or vertical only** (`GraphicsTemplate.cpp:480-483`) | angled + multi-stop + radial gradients, gradient on text |
| Effects | text shadow (blur ≤16, faked with 8 rings), text outline ≤8, node opacity | **shape drop shadow, glow**, inner shadow, backdrop blur ("glass"), blend modes, alpha masks |
| Text | system font, weight 400/500/600/700, size, line height, 1-3 lines, align, ellipsis, auto-shrink, collapse-when-empty | **italic** (hard-coded normal, `Raster.cpp:385`), **tracking**, **all-caps transform**, mixed runs, bundled fonts on Windows (Selawik is Mac-only) |
| Motion | graphic in/out plus per-layer in/out: cut, fade, rise, slide, wipe (rect clip), grow, pop; 4 easings; ≤3000 ms; 3 presets | **mask/shape reveals**, expo/quint/back curves exposed by name, per-word text stagger, **looping accents** (shimmer, sweep, pulse), data-change transitions |

### 2.3 Colour today

Colour is chosen in **seven places, with four different controls and two hex parsers**:

| # | Where | Control | Alpha |
|---|---|---|---|
| 1 | Builder "COLORS" (Accent, Background, Name, Second line) | WinUI `ColorPicker` flyout, `GraphicsBuilderControl.cs:480` | no |
| 2 | Designer Fill / Colour | ComboBox of 7 tokens or typed `#RRGGBB`, `Inspector.cs:273-293` | no |
| 3 | Designer stroke / outline / shadow | plain TextBox, applied on "Apply stroke", `Content.cs:38,76,80` | no |
| 4 | Cues & automation `color` fields | `ColorPicker`, alpha only when `MaxLength != 7`, `GraphicsWorkspaceControl.cs:260` | sometimes |
| 5 | Show starters | accent hex box plus "Pick color…", `Starters.cs:56-62` | no |
| 6 | Brand kit (Primary, Accent, Background) | hex TextBox plus "Pick", `OverlaysPage.xaml:300-380` | no |
| 7 | Legacy lower-third designer | hex TextBox plus picker, `LowerThirdDesignerControl.cs:20-25` | no |

- **Parsers:** `HexColor.cs` reads 6 digits only (opaque). `GraphicsColor.cs` reads 7 or 9 digits (RGBA).
  The core stores ARGB uint32 and accepts `#RRGGBBAA`.
- **`brand.*` tokens are not a brand kit.** They resolve from each row's `LowerThirdAppearance`
  (`GraphicsInstances.cpp:283-292`). The brand kit is copied into a row only when the operator
  presses "Use brand kit colors", and that copy maps BrandColor to TitleColor.
  So changing the brand kit changes nothing already on air or in the playlist.
- **Missing:** eyedropper, recent colours, saved swatches, a palette beyond 3 kit colours,
  alpha in the Builder and Designer, contrast checks, broadcast-safe warnings.
- **Mac:** a single SwiftUI `ColorPicker` (`supportsOpacity:false`) in the legacy brand kit
  (`Overlays.swift:353-371`). There is no colour UI in Graphics, because the Mac Design side is
  still a placeholder (`GraphicsDesignPane.swift:19`).

### 2.4 Stock looks today

All of these are flat: a solid plate, an accent bar, Segoe UI and a fade.

- **Compatibility looks** (drawn natively): `compact-solid`, `minimal-accent`, `broadcast`.
- **Curated templates:** `corporate-lower-third`, `logo-accent`, `transparent-name`,
  `centered-title`, `corner-logo`, `clock-panel` (`GraphicsCuratedTemplates.cpp:13-21`).
- **Blank seeds** (`contracts/graphics-seeds/*.json`): lower-third, title, bug, full-screen.
- **Show starters:** Interview, Panel, Webinar, Branded. These change only the accent colour on a `#172433` base.

### 2.5 Mac status

| Area | Mac |
|---|---|
| Operate (playlist, Cue/Show/Hide, names, clocks, tables, crawls) | Built (G1-G7) |
| Rendering of every Windows template | Built (Core Text raster, Metal upload, thumbnails) |
| Builder, Designer, motion editing, image/SVG import, remote feeds | **Not built**. Design tab shows "arrives in a later release" |
| Private preview (`lowerThirdPreview`) | **Not built**: `MetalGradePreview.mm:159` answers `native-appearance-preview-not-built`. Blocks any Mac Builder |
| Windows parity reference | **Never generated**. `MacMatchesWindowsReference` prints UNVERIFIED (G3-G5 outcomes, Task 7) |
| Known differences | 5 font substitutions only; colour emoji on Mac vs mono on Windows; no faux bold on Mac; gradient alpha blending unconfirmed; long crawls failed on Windows (fixed on branch `codex/win-d3d11-graphics-reference`, not merged) |

## 3. What the competition does

The research brief is reproduced in summary. Sources are in Appendix A.

- **None of vMix, Ecamm, mimoLive, H2R, Captivate, Singular or ProPresenter has a real brand kit**, meaning
  named colour and font tokens set once, inherited by every template, and swappable in one action.
  vMix GT Designer still has no colour picker (forum requests run up to Sept 2025). Ecamm tells users
  to paste hex codes into the macOS colour panel. HTML tools (SPX, CasparCG) have theme files but keep
  them away from the operator.
- **Patterns worth copying:**
  - mimoLive **variants**: one design, many speakers, a hotkey each. mimoLive also has **Text Delay**
    (plate first, text follows) and Frosted Glass backgrounds, with a warning that they are GPU-heavy.
  - Singular **control nodes**: the designer chooses which properties the operator may touch.
  - ProPresenter's two-tier **scripture** (verse plus reference).
  - Captivate's breadth of **data sources**.
- **The gap CoreVideo can own:** brand tokens that cascade into every graphic, an eyedropper on live
  video, and genre packs built on those tokens, so that "make it our colours" takes one action.

## 4. Target: the colour system

### 4.1 Show palette (the brand kit becomes real)

A **show palette** lives in the show file and has named tokens. Every template references tokens,
never raw hex, unless the operator detaches a value.

| Token | Default role | Example (News pack default) |
|---|---|---|
| `brand.primary` | main plate | `#0B1F3A` |
| `brand.secondary` | second plate / title bar | `#13315C` |
| `brand.accent` | strips, tabs, highlights | `#E63946` |
| `brand.onPrimary` | name text on primary | `#FFFFFF` |
| `brand.onSecondary` | title text on secondary | `#D8E2F0` |
| `brand.onAccent` | text on accent (LIVE tab) | `#FFFFFF` |
| `brand.surface` | neutral plate / glass tint | `#0A0F18` at 70% |
| `brand.teamA`, `brand.teamB` | sports only, optional | `#C8102E`, `#003087` |

- **Schema:** add tokens to the existing 10, keeping all current names, so existing templates resolve unchanged.
  Each token is `#RRGGBBAA`.
- **Resolution order:** row override → show palette → pack default.
  This replaces today's per-row derivation from `LowerThirdAppearance`. The compatibility looks keep
  reading their row appearance until they are migrated.
- **Live re-theme rule:** editing the palette updates Cue and Preview immediately. Program changes
  **only on an explicit Update/Show**, per the existing private-Cue contract. The palette editor shows
  "N graphics on air use this colour — Update on air" as a single button.
- **Palette presets:** packs ship palettes, and the operator can **Import from logo**: k-means over a PNG
  suggests primary, accent and surface, and the operator confirms. Saved palettes can be reused across shows.
- **Derived tokens:** `onPrimary` and the other on-colours default to auto, picking white or near-black by
  contrast (the Volt spec already asked for this). The operator can pin any of them.

### 4.2 One colour picker, everywhere

A single control on each platform replaces all seven entry points in 2.3. On Windows it is
`CvColorField` (WinUI UserControl); on Mac it is `ColorField` (SwiftUI, an NSColorPanel bridge, not the
system `ColorPicker`, which cannot do tokens). Both write the same string format.

```
┌ Accent ─────────────────────────────── [● brand.accent ▾] ┐   ← linked-token chip; ▾ = pick token / Detach
│ ┌──────────────────────┐ ┃              #E63946  [100 %]  │   ← hex accepts #RGB, #RRGGBB, #RRGGBBAA
│ │   SV square          │ ┃hue           R 230 G 57 B 70   │   ← scrub-to-adjust numeric fields
│ │                      │ ┃              ▤ alpha slider    │
│ └──────────────────────┘ ┃                                │
│ Palette  ■ ■ ■ ■ ■ ■ ■ ■     Recent ■ ■ ■ ■ ■ ■ ■ ■     │   ← show palette + last 12
│ [⌖ Pick from Program] [⌖ Pick from source…]               │   ← eyedropper on live video
│ ⚠ Name text on this plate: 3.1:1 — below 4.5:1 on air     │   ← contrast + broadcast checks
└───────────────────────────────────────────────────────────┘
```

- **Token-first.** When a property is bound to a token, the field shows the chip and editing it asks:
  "Change brand.accent for the whole show / Detach just this layer".
- **Eyedropper on video, without input injection.** The pick reads a pixel from the core's existing
  Program/source thumbnail readback (a new `colour.sample {source, x, y}` RPC returning sRGB), on a
  frozen frame the operator clicks inside the picker's own preview image. There is no OS-level screen
  picker, so the eyedropper works the same on both platforms and respects the no-input-injection rule.
- **Alpha** is available everywhere the core supports it, and that is every colour.
- **Gradient editor** (once 6.1 lands): stops (add/remove/position/alpha), linear/radial, angle. Each stop
  can be a token.
- **Checks** are non-blocking warnings:
  - **Contrast:** WCAG ratio of text against its plate token. Warn below 4.5:1 for all on-air text,
    stricter than WCAG's 3:1 for large text, because compression and moving video reduce contrast.
    For plate-less styles (lyrics, minimal), the check runs against the shadow/gradient and says
    "legibility depends on the video behind it".
  - **Broadcast:** very saturated red/blue on text under 28 px (chroma bleed after 4:2:0); pure
    `#FFFFFF` over large areas, with an offer to cap at 235; 1 px lines (twitter). An optional
    **Broadcast-safe clamp** toggle at output. Given the limited-range history, we verify
    that the picker's sRGB maps through the same path as the Program (see
    `corevideopro-limited-range-100pct`).

### 4.3 Where colour appears after the change

| Surface | After |
|---|---|
| Graphics → **Palette** (new, top of Design) | edit tokens, presets, import from logo, "update on air" |
| Builder step "Style" | palette swatches plus per-role ColorField; changes write tokens unless detached |
| Designer inspector | every fill/stroke/outline/shadow/gradient stop is a ColorField; Apply buttons removed (live in Cue) |
| Cues & automation, starters, brand kit, legacy LT designer | same ColorField; legacy brand kit becomes a link to the Palette |
| Operate row | read-only colour dots plus "Colours…" for a per-row override |

## 5. Target: the quick lower-third builder

The five-step Builder stays the quick path, as the November 1 ruling requires. The proposal reshapes it
around packs, so a great-looking result is the default.

```
┌ New lower third ──────────────────────────────────────────────────────────────────┐
│ Style   [News] [Sports] [Worship] [Corporate] [Esports] [Talk] [Minimal]          │
│ ┌─────────┐ ┌─────────┐ ┌─────────┐ ┌─────────┐   ← live thumbnails rendered by  │
│ │ Anchor  │ │ Breaking│ │ Reporter│ │ Two-line│      the core in the CURRENT      │
│ └─────────┘ └─────────┘ └─────────┘ └─────────┘      palette, never static art    │
├───────────────────────────────────────────────────────────────────────────────────┤
│ ┌ Preview (private, over Program freeze-frame or a sample image) ──────────────┐ │
│ │  ▶ Play in   ▶ Play out   ⟲ loop                                              │ │
│ └──────────────────────────────────────────────────────────────────────────────┘ │
│ Name  [Jordan Ellis            ]   Title [Senior Correspondent ]  Tag [LIVE ]     │
│ Colours  ● Primary ● Accent ● Text   [Use show palette ✓]                         │
│ Position ◉ Left ○ Centre ○ Right   Safe area ✓ 90 %   Size [M]                    │
│ Motion   [Pack default ▾]  Speed [1.0×]                                           │
│ Variants  + Add speaker   (Jordan Ellis · Sam Ortiz · Priya Rao)  → hotkeys F1-F3 │
│                                       [Save to show]  [Save & Cue]                │
└───────────────────────────────────────────────────────────────────────────────────┘
```

Rules:

- **Live thumbnails** come from the existing worker-rendered thumbnail path (`GraphicsThumbnail` /
  `CoreTextThumbnail`) using the show palette. Changing a colour re-renders the cards, so the operator
  sees all styles in their own brand.
- **Every pack exposes the same three-to-five operator knobs** (Singular control-node model): colours (by
  role), position, size, motion speed, and optional elements (tag/logo/second line). Anything else
  means "Open in Designer".
- **Variants:** one design, many rows. The names list (#1079) already feeds this. "Add speaker" creates a
  playlist row bound to the same design.
- **Safe area:** placement snaps inside graphics-safe (90%, x 96-1824, y 54-1026 at 1080p). The lower-third
  band defaults to y 820-1000.
- **Mac:** the same Builder in SwiftUI. It is **blocked on the Metal private preview** (2.5), so that is
  the first Mac item in section 8.

## 6. Target: style packs

### 6.1 Renderer primitives the packs need (both rasterizers, one change each)

Ordered by how many looks each unlocks:

| # | Primitive | Unlocks | Core schema change | Windows (D2D) | Mac (Core Graphics) |
|---|---|---|---|---|---|
| P1 | **Text tracking, italic, caps transform** | Sports, News, Minimal, Esports | `letterSpacing`, `fontStyle`, `textTransform` on text | `IDWriteTextLayout::SetCharacterSpacing` (via `IDWriteTextLayout1`), `DWRITE_FONT_STYLE_ITALIC` | `kCTKernAttributeName`, italic trait, uppercase before layout |
| P2 | **Angled, multi-stop and radial gradients** | all | `gradient: {type, angle, stops[≤8]}` (old 2-stop form still parses) | `ID2D1LinearGradientBrush` / `RadialGradientBrush` with N stops | `CGGradient` N stops, `drawLinear/RadialGradient` |
| P3 | **Polygon / skew plate** | Sports, Esports, News tabs | `shape: "polygon"` with ≤16 points in box units, or `skewX` on rect | `ID2D1PathGeometry` | `CGPath` |
| P4 | **Shape drop shadow and glow** | all premium looks | `shadow: {color, dx, dy, blur, spread}` on any node; glow = shadow at 0,0 | D2D `Shadow` effect needs a device context; the raster uses a WIC render target, so implement as box-blurred alpha copy (same ring method as text, or a 3-pass box blur) | `CGContextSetShadowWithColor` gives true Gaussian. **Parity risk**: match Windows' blur model with a fixture tolerance |
| P5 | **Mask reveal motion** | News wipes, Minimal text reveal | motion type `reveal` with direction and a feather | clip to animated rect per frame (already have wipe); add soft edge | same |
| P6 | **Per-word / per-line stagger** | every pack's polish | `stagger: {by: "line"\|"word", ms}` on a text layer | atlas slices per line exist; extend unit split | same, shared core |
| P7 | **Looping accent** | News shine sweep, Esports pulse, Worship drift | `loop: {type: "sweep"\|"pulse"\|"drift", periodMs}` on a layer | per-frame transform/opacity of a pre-rastered slice (no re-raster) | same |
| P8 | **Bundled fonts on Windows** | consistent packs on both | pack ships OFL fonts; `fonts/` in package | private font collection via `IDWriteFactory5::CreateInMemoryFontFileLoader` (or the existing collection loader extended to the pack dir) | already loads `COREVIDEO_GRAPHICS_FONTS_DIR`, extend to pack dir |
| P9 | **Backdrop blur ("glass")** | Worship, Corporate | `backdrop: {blur, tint}` | needs a GPU pass in the compositor over Program, not in the overlay raster | Metal MPS blur pass |

- **P1-P4 are the minimum for packs that do not look flat.**
- P5-P7 add motion polish.
- P9 is the most expensive. It runs on the compositor's GPU path, it is a frame-budget risk at 1080p60,
  and mimoLive warns about the same cost. **Owner ruling (2026-11-27): true backdrop blur is required.**
  No frosted approximation ships. Glass looks wait for P9 on both Windows and Mac, and P9 must hold the
  outputs release gate (zero dropped frames at 1080p60) with two pack graphics plus a crawl on air.

Each primitive carries:
- a parity fixture in `native/tests/fixtures/graphics-parity/`;
- a Mac golden;
- a Windows reference row, which needs the Windows reference from Task 7 first;
- a contract case if it changes preparation (atlas size or admission).

### 6.2 Pack format

A pack is a folder (later a `.cvpack` zip) that installs through the existing
`graphics-template-install` command:

```
packs/news-signal/
  pack.json        { id, name, genre, version, palettes:[...], fonts:[...], templates:[...] }
  palettes/        default.json, dark.json, …  (token → #RRGGBBAA)
  templates/       anchor.json, breaking.json, reporter.json, two-line.json, bug.json, ticker.json
  fonts/           OFL .ttf/.otf + OFL.txt
  assets/          PNG textures only (no SVG on Mac yet; ≤2 MB per pack)
```

Rules:
- Templates reference **tokens and pack fonts only**. A lint (CI) rejects raw hex in a pack template
  unless it is marked `"fixed": true`. Fixed colours suit things like a white LIVE dot or a halftone texture.
- Every template stays inside today's limits (64 layers, 32 fields, ≤3000 ms motion).
- Each template has a **parity fixture**. The pack builds into the golden suite, so a pack can't ship
  if it renders differently on Mac and Windows beyond tolerance.
- Packs are core data. The Windows and Mac shells get them with no shell code.

### 6.3 The launch packs

Each pack has: lower third (1-line and 2-line), name plus tag variant, full-width title,
corner bug and, where the genre needs it, one extra. Fonts are Google Fonts / OFL, bundled.
Timings are practitioner conventions.

| Pack | Look | Type | Motion | Extra |
|---|---|---|---|---|
| **News: Signal** | stacked name bar over secondary title bar; hard accent strip at left; red LIVE/BREAKING tab; subtle top-down gradient and a 1 px highlight line; a shine sweep crosses every 8 s | Inter / Barlow Semi Condensed: name 600, title 400, tag 700 caps tracked +6% | strip wipes up (200 ms) → plate reveals left-to-right (400 ms, expo-out) → text 100 ms later per line; out 70% of in | ticker / crawl skin, clock bug |
| **Sports: Kinetic** | 12° skewed plates in team colour; dark number/stat block; diagonal speed-line texture at 15% | Barlow Condensed / Oswald, Bold Italic caps, tight tracking | plates slam in from the side (250 ms, back-out overshoot), staggered 60 ms; text snaps | score bug (team A/B, score, clock, period), stat lower third |
| **Worship: Grace** | no hard box: soft dark gradient rising from the bottom, thin gold rule, optional glass plate (true backdrop blur, P9) | Cormorant Garamond for scripture / headings, Montserrat Light for names | 800 ms dissolve, sine in-out; gentle 6 px rise; text follows 150 ms | **Scripture** (verse + reference, 2-tier, up to 4 lines), **Lyrics** (lower-centre, no plate, shadow only) |
| **Corporate: Clarity** | rounded 8 px plate, brand-primary accent strip, optional logo on the left, soft shadow | Inter / Montserrat 600 / 400 | slide + fade 450 ms, quint-out; text stagger 80 ms | webinar title card, speaker with company |
| **Esports: Neon** | angular HUD frame (polygon with cut corners) on near-black; accent glow; scanline texture | Rajdhani / Chakra Petch caps | 250 ms, stepped "glitch" in (3 opacity steps), pulse loop on the accent | player card, caster duo |
| **Talk: Studio** | two-tone pill (name pill + role pill), handle line with social icon | DM Sans / Poppins | spring pop 400 ms (back-out), pills stagger 90 ms | guest + handle, "Up next" |
| **Minimal: Line** | no plate: hairline rule and large weight contrast | Inter 700 / 300, wide tracking on title | text mask reveal 500 ms, quint-out | single-line name, quote |

Each pack ships 2-3 palettes. News has Navy/Red, Charcoal/Gold and White/Blue. Sports defaults to
team tokens. Worship has Warm, Midnight and Light.

The companion mockup page draws these seven looks over sample video, with a live show palette that
recolours every pack: https://claude.ai/artifact/LGEfwMfytp46ctDsG7a3wU (private to the owner).
The mockups use browser effects; the tags under each frame name the primitives (6.1) the real
renderer needs first.

## 7. Parity: one pack, identical on both platforms

- **Prerequisite (in flight):** the Windows reference for the G4 parity standard, branch
  `codex/win-d3d11-graphics-reference`:
  - crawl admission fix;
  - prepare-hook seam;
  - `D3D11GraphicsReferenceTest` built;
  - reference written.

  Until it merges, Mac-vs-Windows parity is unproven, and no pack should claim cross-platform identity.
- **For every new primitive (6.1) and every pack template:** add a fixture to
  `native/tests/fixtures/graphics-parity/index.json`, regenerate the Mac golden (Task 6 command) and the
  Windows reference (Task 7 step 5), and judge with the existing comparator.
- **Font rule:** packs bundle their fonts. When a pack font is missing, the app substitutes and warns,
  never fails on air. Strict mode stays for tests.
- **Known differences to settle before packs ship:**
  - gradient alpha blending (spec 5.4, unconfirmed);
  - faux bold (Windows yes, Mac no). Packs must ship every weight they use;
  - the shadow blur model (P4).

## 8. Delivery plan (proposed slices)

Each slice states its platforms. **W** = Windows, **M** = Mac, **C** = shared core (both).

| # | Slice | Platforms | Depends on | Size |
|---|---|---|---|---|
| 0 | Windows parity reference (in flight) | W | — | S |
| 1 | Show palette in the show file; `brand.*` resolve from palette; "update on air"; migration from brand kit | C + W + M (model only on M) | — | M |
| 2 | `CvColorField` (W) / `ColorField` (M); replace the 7 Windows entry points; contrast and broadcast checks | W, M | 1 | M |
| 3 | `colour.sample` RPC and eyedropper on frozen Program/source frame | C + W + M | 2 | S |
| 4 | P1 text tracking/italic/caps, plus fixtures | C + W + M raster | 0 | S |
| 5 | P2 gradients (angle, multi-stop, radial) plus gradient editor in ColorField | C + W + M | 0, 2 | M |
| 6 | P3 polygon/skew, plus P4 shape shadow/glow | C + W + M | 0 | M |
| 7 | P8 bundled fonts on Windows; pack format plus installer plus pack lint | C + W | 4 | M |
| 8 | P5-P7 motion polish (reveal, stagger, loops) | C + W + M | 0 | M |
| 9 | P9 true backdrop blur (required; must pass the 1080p60 outputs gate) | W + M compositor | 0 | L |
| 10 | Packs wave 1: **all seven** (News, Sports, Worship, Corporate, Esports, Talk, Minimal): templates, palettes, fixtures, goldens | data (both) | 4-9 | L |
| 11 | Builder reshaped around packs (live palette thumbnails, variants, safe area) | W | 10 | M |
| 12 | Metal private preview (`lowerThirdPreview`) | M | — | M |
| 13 | Mac Builder (SwiftUI) on the same pack/palette model | M | 2, 10, 12 | L |
| 14 | Import-from-logo palette; pack import/export (`.cvpack`) | W + M | 10 | M |

Status: slice 1 is implemented on Windows and as a Mac model (see
[graphics playout: Show palette](graphics-playout.md#show-palette-graphics-styles-slice-1)).
Owner acceptance 2 (section 9) has not been performed.

Owner-visible milestones:
- **After slice 3:** one picker everywhere, colours set once.
- **After slice 11:** all seven packs on Windows.
- **After slice 13:** the same on Mac.

## 9. Acceptance (owner-performed, per the usability gate)

1. Set a show palette, then go to air with a News lower third in **under 2 minutes** from a fresh show,
   without opening the Designer. Timed by the owner. An agent rehearsal is technical evidence only.
2. Change `brand.accent` once. Cue and Preview recolour immediately. Program changes only after one
   explicit "Update on air".
3. Eyedropper picks a colour from a live camera frame on Windows and Mac.
4. The same pack file renders on Windows and Mac within the parity comparator tolerance, with every
   fixture green on both.
5. Every output has zero dropped frames with two pack graphics plus a crawl on air at 1080p60 (outputs
   release gate). Not tested on a loaded machine only.

## 10. Owner decisions (ruled 2026-11-27)

1. **Palette scope:** per show, seeded from an app-wide default palette for new shows.
2. **Launch pack set:** all seven packs ship in wave 1 (News, Sports, Worship, Corporate, Esports, Talk,
   Minimal). Wave 1 therefore depends on motion polish (P5-P7) and backdrop blur (P9).
3. **Backdrop blur (P9):** true GPU backdrop blur is required on Windows and Mac. No frosted
   approximation ships.
4. **Live re-theme:** a palette change updates Cue and Preview immediately. Program changes only on an
   explicit "Update on air", which matches the private-Cue contract.
5. **Pack distribution:** built-in packs first. Pack import/export (`.cvpack`) comes in slice 14.

## Appendix A: sources

- vMix: [Title help](https://www.vmix.com/help25/Title.html), [GT Designer](https://1.vmix.com/help27/GTTitleDesigner.html), [colour picker request](https://forums.vmix.com/posts/t24926-Colour-Picker-in-GT-Title-Designer), [GT colour requests](https://forums.vmix.com/posts/m96684-GT-Color-feature-requests)
- Ecamm: [brand colours](https://support.ecamm.com/en/articles/3660532-how-to-add-your-branded-colors-to-your-live-streams), [graphics FAQ](https://support.ecamm.com/en/articles/4121614-ecamm-and-graphics-faq)
- mimoLive: [Lower Third layer](https://mimolive.com/user-manual/live-editing/layers/lower-third), [variants](https://mimolive.com/user-manual/layers-and-variants)
- H2R Graphics: [site](https://h2r.graphics/), [settings](https://heretorecord.gitbook.io/h2r-graphics/settings)
- NewBlue Captivate: [product](https://newbluefx.com/captivate/)
- Singular: [Control Node](https://support.singular.live/hc/en-us/articles/360001029111-Control-Node)
- ProPresenter: [Themes](https://support.renewedvision.com/hc/en-us/articles/34551484745875-Guide-to-Using-Themes-in-ProPresenter), [Bibles](https://support.renewedvision.com/hc/en-us/articles/360041347594-Using-Bibles-in-ProPresenter)
- SPX: [theme files](https://intercom.help/spx-graphics/en/articles/9523664-theme-files); Loopic: [loopic.io](https://loopic.io/); CasparCG: [HTML templates](https://github.com/CasparCG/help/wiki/Media:-HTML-Templates)
- Safe areas: [EBU R 95](https://tech.ebu.ch/docs/r/r095.pdf), [ITU-R BT.1848](https://www.itu.int/dms_pubrec/itu-r/rec/bt/R-REC-BT.1848-1-201510-I!!PDF-E.pdf)
- Levels: [EBU R 103 changelog](https://tech-qual.ebu.ch/publications/r103/changelog)
- Motion: [Vevo motion system](https://brand.vevo.com/motion/), [Vimeo on lower thirds](https://vimeo.com/blog/post/what-is-lower-thirds); contrast: [WCAG 1.4.3](https://www.w3.org/WAI/WCAG21/Understanding/contrast-minimum.html)

Caveats from the research:
- vMix v28/29 picker status is from forum evidence only.
- H2R global theme cascade and Singular theming could not be confirmed from primary docs.
- All animation timings are convention, not standard.
