# CoreVideo Pro Live Graphics Engine
## Product Spec & Implementation Plan

**Working name:** Volt Graphics  
**Status:** Spec for implementation inside CoreVideo Pro  
**Date:** 2026-10-16  
**Owner:** CoreVideo Pro media core + operator shell  

---

## 1. Purpose

Build a native live graphics system inside CoreVideo Pro that competes with NewBlue Captivate for lower thirds, titles, bugs, crawls, tables/leaderboards, clocks, and data-driven overlays — without leaving the product.

Graphics render directly in the existing C++ media core’s GPU compositor (D3D11 / Metal) as scene-graph layers. There is no external process, NDI hop, or second application.

### Goals
- Professional on-air graphics (lower thirds, titles, bugs, crawls, tables, clocks, CTAs) that look broadcast-quality.
- Data-driven updates from Zoom participant metadata, spreadsheets, JSON, RSS, and clocks with no animation restart on identical values.
- Brand-kit aware templates that automatically adopt logo, colors, fonts, and backgrounds.
- Operator controls that live in the existing WinUI / SwiftUI / React shell via the typed command/snapshot protocol.
- Integration with Magic Scene and Set & Forget so graphics can be auto-inserted or suggested.
- Incremental delivery: useful lower thirds and bugs in the first milestone; full Captivate-class capability later.

### Non-Goals (MVP)
- Full After Effects round-trip or complex 3D scene authoring.
- Unlimited simultaneous live layers with arbitrary shader graphs (start with a practical budget, e.g. 8–16).
- Sports scoreboard protocol adapters (Daktronics serial, StatCrew, etc.) — design the controller interface so they can be added later.
- Separate standalone application or NDI-only output path (optional export can come later).

---

## 2. Competitive Positioning vs NewBlue Captivate

| Capability | Captivate | Volt Graphics (target) |
|------------|-----------|------------------------|
| Lower thirds, titles, bugs, crawls | Yes | Yes (native) |
| Data controllers (sheets, JSON, RSS, clocks) | Yes | Yes |
| Zoom/Teams chat & participant data | Yes | Yes (native Zoom metadata first) |
| Template library + designer | Yes | Yes (JSON templates + parameter editor) |
| Output | NDI / SDI key-fill / HDMI / OBS | Direct compositor layers (optional NDI later) |
| Latency | Extra hop | Same frame as video |
| Brand consistency | Manual | Automatic brand-kit binding |
| AI director integration | None | Magic Scene / Set & Forget |
| Control | Own UI + Companion | Same console + existing Stream Deck / OSC / TCP |
| Cost | Separate subscription / perpetual | Included in CoreVideo Pro |

---

## 3. Architecture

### 3.1 Process & Ownership Boundaries
Follow the existing four-process model and the load-bearing rule: **the shell owns no real-time media**.

- **C++ media core** (`corevideo-native.exe`): owns all graphics state, data binding evaluation, animation timing, rasterization, and compositing.
- **Shell** (WinUI 3 / SwiftUI): owns operator intent (trigger, edit parameters, select template) and displays shared textures + snapshots. No pixel work.
- **Zoom engine**: continues to supply participant metadata; graphics consume it via the existing snapshot path.
- Graphics instances become additional layers in the existing scene-graph / render-plan that `MediaCore::buildCompositorRenderPlan` already walks.

### 3.2 Data Flow
```
Data sources
  Zoom metadata · XLSX/CSV · Google Sheets · JSON/XML · RSS · clocks · custom HTTP/WebSocket
       ↓
Controllers (poll or subscribe, rate-limited)
       ↓
Binding engine (field → parameter, change detection)
       ↓
Active graphic instances (template + resolved parameters + animation state)
       ↓
Scene-graph / render-plan layers
       ↓
GPU compositor (D3D11 / Metal)
       ↓
Program / Preview shared textures → encoder / recorder / optional NDI
```

### 3.3 Core Components (new modules inside native/)

| Module | Responsibility | Notes |
|--------|----------------|-------|
| `GraphicsEngine` | Owns template registry, instance lifetime, animation clock, layer ordering | Command-driven; publishes snapshot state |
| `TemplateLoader` | Loads JSON template packages + assets | Signature-cached |
| `BindingEngine` | Evaluates controllers → parameters; structural change detection | Avoids animation restarts (same pattern as OverlayDirector) |
| `Controllers` | ZoomMetadata, Spreadsheet, JsonHttp, Rss, Clock, Custom | Pluggable; start with Zoom + Clock + JSON |
| `Rasterizer` | Text (DirectWrite / Core Text), images, simple shapes | Extends existing `OverlayTileRaster` / DirectWrite path |
| `AnimationRuntime` | Reveal/hide, loop, simple keyframes, crawl offset | GPU-friendly uniforms where possible |
| `GraphicsRenderPlan` | Emits layers into the existing compositor render plan | Safe-region and collision awareness |

### 3.4 Template Format (JSON)
A template package is a folder or zip containing `template.json` + assets.

```json
{
  "id": "lower-third-corporate-01",
  "name": "Corporate Lower Third",
  "type": "lower-third",
  "version": 1,
  "canvas": { "safeRegion": "lower-third" },
  "layers": [
    {
      "id": "bar",
      "type": "shape",
      "geometry": { "x": 0, "y": 0, "w": "100%", "h": 72 },
      "style": { "fill": "@brand.primary" }
    },
    {
      "id": "name",
      "type": "text",
      "text": "{{name}}",
      "style": { "font": "@brand.fontHeading", "size": 32, "color": "@brand.onPrimary" }
    },
    {
      "id": "title",
      "type": "text",
      "text": "{{title}}",
      "style": { "font": "@brand.fontBody", "size": 20, "color": "@brand.onPrimaryMuted" }
    }
  ],
  "animations": {
    "in": { "type": "slide", "from": "left", "durationMs": 400 },
    "out": { "type": "fade", "durationMs": 300 }
  },
  "fields": [
    { "id": "name", "type": "string", "bindable": true },
    { "id": "title", "type": "string", "bindable": true }
  ]
}
```

Brand tokens (`@brand.primary`, `@brand.fontHeading`, etc.) resolve against the existing brand kit at instance time.

### 3.5 Graphic Instance State
Each live instance has:
- `id`, `templateId`, `layerOrder`
- Resolved parameters (after binding)
- Animation state (`hidden` | `in` | `on-air` | `out`)
- Visibility, hold, auto-hide timer
- Collision / safe-region constraints

### 3.6 Commands & Snapshots
Extend the existing JSON-line protocol.

**Commands (shell → core)**
- `graphics.template.load` / `graphics.template.list`
- `graphics.instance.create` (templateId, initialParams, targetScene)
- `graphics.instance.update` (params)
- `graphics.instance.trigger` (in / out / toggle / clear)
- `graphics.instance.setOrder`
- `graphics.controller.add` / `graphics.controller.remove`
- `graphics.playlist.play`

**Snapshots (core → shell)**
- Active instances (id, template, resolved params, animation state, errors)
- Controller health (last update, error)
- Template catalog summary

### 3.7 Integration Points with Existing Code
- **Compositor**: Graphics layers are added to the render plan alongside video, borders, and existing overlays. Reuse safe-region math.
- **Overlay rasterizer**: Extend `OverlayTileRaster` / DirectWrite path for multi-line text, shapes, and simple images. Keep the content-signature cache.
- **Brand kit**: Resolve `@brand.*` tokens at instance creation and on brand-kit change.
- **Magic Scene / Director**: Optional rules that auto-create or suggest lower thirds when a participant becomes featured.
- **OverlayDirector (TS)**: Can be retired or kept as a pure derivation helper; the authoritative state moves to the C++ engine.
- **Lower-third deck (existing TS)**: Migrate queue/show semantics into the GraphicsEngine.

### 3.8 Performance Constraints
- All raster and animation work stays on the GPU path or a dedicated worker; no per-frame pixel work in the shell or on the command thread.
- Signature caching for text/image raster (already proven in the DirectWrite path).
- Change detection so identical parameter updates do not restart animations.
- Practical simultaneous layer budget for MVP (8–16); measure and raise later.
- 1080p60 and 4K must remain viable; graphics are composited before hardware encode.

---

## 4. Feature Scope by Phase

### Phase 0 — Foundation (1–2 weeks)
- GraphicsEngine skeleton inside the media core.
- JSON template loader + brand-token resolver.
- Extend existing overlay rasterizer for multi-line text + simple rectangles.
- Basic instance create/update/trigger commands and snapshot.
- One hard-coded lower-third template driven by Zoom name + role.
- Operator UI: simple trigger buttons and parameter override in the shell.

**Exit criteria:** Operator can show/hide a branded lower third that updates from Zoom metadata without animation restart; renders in program and preview.

### Phase 1 — Useful Graphics (2–3 weeks)
- Template catalog (10–15 lower thirds, bugs, simple titles).
- Parameter editor in the shell.
- Clocks and countdowns.
- Static bugs / logos with fade in/out.
- Auto-reveal/hide tied to featured participant (existing director signals).
- Hotkey and Stream Deck support via existing control surfaces.
- Collision avoidance with captions and faces (reuse existing safe-region logic).

**Exit criteria:** Operator can build a typical corporate/webinar show with lower thirds, bug, and clock using only CoreVideo Pro.

### Phase 2 — Data-Driven (2–3 weeks)
- Controllers: local XLSX/CSV, JSON HTTP polling, RSS.
- Tables and simple leaderboards with row add/remove animation.
- Binding UI (map column → field).
- Crawl / ticker (text + optional image segments).
- Playlist / sequence playback.

**Exit criteria:** Operator can drive a table or crawl from a spreadsheet or JSON endpoint and update it live.

### Phase 3 — Captivate Parity (3–4 weeks)
- Full template library (50–80 items covering corporate, education, worship, events).
- Media-rich crawls.
- Animated leaderboards with smooth transitions.
- Improved designer (layer visibility, simple animation timing).
- Optional NDI key/fill or alpha export for hybrid workflows.
- Bitfocus Companion preset pack.

**Exit criteria:** CoreVideo Pro can replace Captivate for the majority of non-sports live productions.

### Phase 4 — Differentiation (ongoing)
- Sports protocol adapters (if demand exists).
- PSD / SVG import.
- AI-assisted template suggestion from Magic Scene.
- Media bin integration with CoreVideo Volt MAM.
- Limited After Effects JSON intermediate import.

---

## 5. Implementation Plan

### 5.1 Immediate Next Steps (Week 1)
1. Create `native/src/graphics/` module skeleton (`GraphicsEngine.h/.cpp`, `Template.h`, `Instance.h`).
2. Define the JSON template schema and a single reference lower-third template.
3. Extend the command/snapshot protocol with the Phase 0 commands.
4. Hook one graphics layer into `buildCompositorRenderPlan` using the existing raster path.
5. Add a minimal shell panel (trigger + name/title override) that sends commands and reads the snapshot.
6. Wire Zoom participant name + role into the binding engine.

### 5.2 Ownership & Review
- GraphicsEngine and raster changes: native core team.
- Protocol contracts: shared contracts/ directory (generate C++ / C# / Swift models).
- Shell UI: WinUI / React operator surface.
- Every phase ends with a short operator acceptance pass (lower third on program, data update without glitch, brand kit applied).

### 5.3 Risks & Mitigations
| Risk | Mitigation |
|------|------------|
| Raster path not yet production-ready on both platforms | Start with the proven CPU + DirectWrite path; Metal text can follow the existing TODO pattern |
| Animation complexity explodes scope | Limit Phase 0–1 to slide/fade; keyframes only in Phase 3 |
| Template authoring bottleneck | Ship a small high-quality set; make the JSON format easy for external designers |
| Shell ViewModel bloat | Keep all state authoritative in the core; shell only sends intent and binds snapshot |
| Performance at 4K60 | Signature cache + GPU upload; measure early |

### 5.4 Success Metrics
- Time to first lower third on air from a cold start: < 30 seconds.
- Data update latency (spreadsheet → on-air text): < 500 ms.
- No animation restart on identical parameter updates.
- Graphics remain in sync with program/preview Take.
- Operator can produce a typical webinar/interview look without leaving CoreVideo Pro.

---

## 6. Open Questions
- Preferred product name (Volt Graphics, CoreGraphics, ProTitles, or simply “Graphics” inside CoreVideo Pro)?
- Should the first templates be fully hard-coded in C++ for speed, or is a minimal JSON loader required from day one?
- Priority of sports adapters vs. general corporate/education/worship templates?
- Desired simultaneous layer budget for the first production release?

---

## 7. Summary
Volt Graphics is not a separate product. It is an extension of the existing C++ media core, scene graph, overlay rasterizer, brand kit, and Magic Scene director. The first useful version (branded lower thirds + bugs + clocks driven by Zoom metadata) can be built by extending seams that already exist. Subsequent phases add data controllers, tables, crawls, and a template library until CoreVideo Pro can replace Captivate for the majority of live productions that currently need it.
