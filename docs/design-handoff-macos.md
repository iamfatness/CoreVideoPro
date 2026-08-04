# CoreVideo Pro design handoff — macOS port

This is the platform-neutral spec of the Windows app's look and feel ("broadcast
console" design language). It exists because the Mac session cannot see the running
Windows app — treat this document plus the reference screenshots as ground truth,
NOT macOS platform defaults. Sources of truth on the Windows side:
`native-shell/CoreVideoPro.WinUI/Themes/StudioTheme.xaml`,
`native-shell/CoreVideoPro.WinUI/App.xaml` (the `Studio*` brush block), and
`native-shell/CoreVideoPro.WinUI/Views/OperatorTabResources.xaml`.

## Non-negotiables

1. **Dark only.** The app is a fixed dark broadcast console. Do not honor the system
   light/dark appearance; force dark. No macOS vibrancy/translucency/materials —
   surfaces are opaque hex colors below.
2. **Bundle the fonts.** Space Grotesk (display/UI) and IBM Plex Mono (labels,
   telemetry, numeric values), both OFL-licensed, are bundled in
   `native-shell/CoreVideoPro.WinUI/Assets/Fonts/` (SpaceGrotesk.ttf,
   IBMPlexMono-Regular/-Medium/-SemiBold.ttf). Copy those exact TTFs into the Mac
   bundle and register them. If the Mac build falls back to SF Pro / Menlo / Helvetica
   it will never look right no matter what else matches.
3. **No stock controls.** Every visible control is custom-styled to the specs below.
   A default-styled NSButton/SwiftUI button, system focus ring, or Aqua accent color
   is a bug.
4. **Status colors are load-bearing** — they encode live-production state and must
   never be repurposed or "tastefully adjusted":
   - Live / accent green `#22C86E`
   - Program / warning amber `#E8A41F`
   - On-air / record red `#E5433F`

## Color tokens

| Token | Hex | Use |
|---|---|---|
| Background | `#0A0B0C` | window/app background |
| Panel | `#101315` | panels, sidebars, section containers |
| Surface | `#16191B` | raised controls |
| Surface raised | `#1B1F22` | one step above surface (hover, popovers) |
| Field / tile well | `#0E1112` | text-field backgrounds, inset tile wells |
| Border | white @ 9% (`#FFFFFF` α 0.09) | control borders (1 px) |
| Line2 (subtle divider) | white @ 5% (`#FFFFFF` α 0.05) | hairline dividers |
| Text primary | `#E9EDEF` | primary text |
| Text secondary / muted | `#8B949B` | body copy, placeholders, mono labels |
| Text dim (tertiary) | `#5C656B` | dimmed mono labels |
| Accent / live green | `#22C86E` | accent buttons, live indicators |
| On-accent text | `#06170D` | text on accent-green fills |
| Program amber | `#E8A41F` | program/tally, warnings |
| Air red | `#E5433F` | on-air, record |

## Typography

- **Space Grotesk** — all UI text. Section titles: 14 px SemiBold. Body: 12 px
  Regular in text-secondary, word-wrapping. Button labels: 13 px SemiBold
  (nav buttons 12 px).
- **IBM Plex Mono** — field labels, timecodes, meter values, badges, telemetry:
  - *Mono label*: 10 px, letter-spacing 0.16 em, muted (or dim variant `#5C656B`).
    Labels are authored UPPERCASE (uppercase the string; no small-caps feature).
  - *Mono value*: 12 px Medium, letter-spacing 0.02 em, text primary.
  - *Mono tag/badge*: 10 px SemiBold, letter-spacing 0.12 em.

## Brand & logo

- **The brand mark is the "Multiview" mark**: a rounded 16:9 monitor frame divided
  2×2 with the top-right (live) tile filled. It is pure vector — the canonical
  platform-neutral copy is `docs/design-reference/multiview-mark.svg`, transcribed
  1:1 from `Controls/MultiviewMark.xaml` (120×120 viewBox: frame rect 14,22 92×76
  r15 stroke 6; cross lines at x=60 and y=60 stroke 5; live tile 65,27 36×28 r6).
  Default color is live green `#22C86E` for both stroke and tile. State variants
  change ONLY the tile fill: program amber `#E8A41F`, on-air red `#E5433F`. Never
  redraw, restyle, or "macify" this mark — reuse the exact geometry.
- **App-icon treatment** (title bar and dock/app icon): the mark sits inside a
  36×36 squircle, corner radius 9, padding 6, filled with a near-black vertical
  gradient `#15191B` → `#0B0D0E` (slight rightward tilt, StartPoint 0,0 →
  EndPoint 0.35,1). The macOS .icns should be built from this same composition
  (mark on the gradient squircle) — Windows packaged icons live at
  `native-shell/CoreVideoPro.WinUI/Assets/` (AppIcon.ico, StoreLogo.png, etc.)
  for reference.
- **Wordmark**: the product name is always "CoreVideo Pro" set in Space Grotesk
  SemiBold (14 px in the title bar) with the subtitle "Live production" at 11 px
  in muted `#8B949B` beneath it. No custom lettering — the wordmark IS the font.

## Component specs

- **Ghost button** (default button): transparent fill, 1 px border (white @ 9%),
  8 px corner radius, padding 14×9, min-height 38, 13 px SemiBold, text primary.
- **Accent button** (primary action): `#22C86E` fill, text `#06170D`, no border,
  8 px radius, padding 16×10, min-height 40, 13 px SemiBold.
- **Nav button**: ghost button but padding 12×8, min-height 36, 10 px radius, 12 px.
- **Text field**: `#0E1112` fill, 1 px border (white @ 9%), 8 px radius,
  padding 10×8, 12 px text, placeholder in muted `#8B949B`.
- Corner radius vocabulary is 8 px (controls) and 10 px (nav/pills). Nothing is
  fully rounded, nothing is square.
- **Known inconsistency (document, don't invent):** sliders, toggle switches, and
  progress-style controls in the current Windows build render with the stock WinUI
  light-blue accent (see the Audio page reference screenshot) — they were never
  restyled to the green accent. On the Mac, match the reference screenshots as-is;
  if the owner later decides to unify them on `#22C86E`, that will be a deliberate
  cross-platform change, not something the port should do unilaterally.

## Verification protocol (how "done" is judged)

Reference screenshots of the running Windows app live in `docs/design-reference/`
(captured 2026-08-03 at 1760×1040, one per nav page: 01 Studio, 02 Zoom, 03 Sources,
04 Routing, 05 Overlays, 06 Audio, 07 Media, 08 Automation, 09 Health; plus
`multiview-mark.svg`, the canonical brand mark). For every Mac screen:

1. Screenshot the Mac build (`screencapture -x` or the window-id variant).
2. Put it side-by-side with the Windows reference at the same logical size.
3. Check in order: background/panel hexes (eyedropper them — they must be exact),
   fonts actually rendering as Space Grotesk / IBM Plex Mono (not a fallback),
   uppercase 0.16 em mono labels, button shapes (radius/border/padding), status
   colors untouched.

Do not claim a screen matches without the side-by-side. If a reference screenshot
for a screen is missing, ask for it rather than guessing.
