# CoreVideo Pro Titler Service — Spec & Implementation Plan

**Status:** Design document (October 2026)  
**Goal:** Deliver a professional, flexible titler service inside CoreVideo Pro that matches or exceeds NewBlue Captivate (Titler Live) for live production needs, while leveraging CoreVideo Pro’s Zoom-native architecture and native GPU compositor.

This is the product and engineering plan for the titler. It is not a claim that the work is already implemented.

## Why this exists

CoreVideo Pro already composites lower-thirds and graphics templates in the native core and generates auto lower-thirds from Zoom participant metadata. The next step is a full titler service that is flexible enough for any professional production (corporate, sports, worship, news, events) without forcing users into an external application.

**Competitive context:** NewBlue Captivate (formerly Titler Live) is the primary reference. Its strengths are a visual Title Designer, deep data binding, multi-layer playout, crawls, and broad I/O. Its weaknesses for CoreVideo Pro’s market are the separate process, edition gating, and limited native Zoom awareness. A native titler removes the context switch and the external hop.

**Differentiation:**
- Zoom participant variables (name, role, company, talking state, etc.) as first-class data sources.
- Direct GPU composite into program/preview (zero external hop for the primary path).
- Unified operator console (playlist + designer live in the same app as scenes and sources).
- Same designs usable by Magic Scene automation.

## Architecture

CoreVideo Pro is four cooperating processes. The titler respects the existing boundaries:

| Component | Owner | Responsibility |
|-----------|-------|----------------|
| Layer graph, animation evaluation, data binding resolution, GPU text/shape rendering, compositing | C++ media core (`native/`) | Real-time pixels. Extends the proven lower-third / overlay path. |
| Playlist, hotkeys, Stream Deck surface, visual designer, data-binding UI | WinUI 3 shell + React operator UI | Operator intent. Never touches media frames. |
| Commands and state | Typed JSON-line IPC | Existing contract pattern. |
| Primary output | Direct composite into program/preview buses | Same path as today’s graphics. |
| Secondary outputs | NDI fill+key, SDI key/fill | For external switchers. |

HTML/CSS/JS templates (embedded browser or GPU path with play/update/stop hooks) are the advanced escape hatch for designs the visual designer cannot express.

## Feature requirements

### 1. Template & design engine
- Visual designer: layers list, canvas, property inspector, timeline with keyframes and easing.
- Elements: text (GPU, multi-style, outlines, shadows, glows), shapes, images, video clips, live video sources (Zoom/camera/NDI mapped into the graphic), groups.
- Relative layout / object following: elements auto-size and reposition when bound text or data changes length.
- Nesting and grouping.
- Imports: PSD (layers preserved), SVG/EPS, Lottie or Rive (optional), PNG sequences.
- Brand kits (fonts, colors, logos, style presets).
- HTML/CSS/JS escape hatch.

### 2. Data binding
Any property (text, color, position, visibility, image source, etc.) can be bound to a variable. Multiple data sources per graphic.

**Data sources (priority order):**
- Zoom participant metadata (name, role, company, talking state, mute, spotlight, breakout, feed quality).
- Clocks, countdowns, timers.
- Local/cloud spreadsheets (Excel, Google Sheets, CSV), JSON, XML, RSS.
- Custom REST/WebSocket/JSON endpoints.
- Tables / leaderboards / crawls driven by data rows.
- Social chat (Zoom/Teams, YouTube, X, etc.) and sports adapters as later modules.
- Internal CoreVideo state (audio levels, scene names, recording status).

Multi-entry / spreadsheet-style variants of the same template so operators can queue dozens of lower-thirds or stats without duplicating graphics.

### 3. Animation & motion
- In/out transitions plus continuous/live updates that do not force a full re-render.
- Preset animation library + full keyframe control.
- Crawls/tickers and rolling credits.
- Ability to map live video into a graphic element with masking.
- Shot layouts / macros: one button takes a group of graphics, sets data values, and optionally triggers scene or audio changes.

### 4. Playout & operator control
- Playlist / channel model with clear preview vs program, Take, and layer order.
- Practical unlimited layers (or a high ceiling) with blend modes and z-order.
- Hotkeys, Stream Deck, MIDI, OSC, and HTTP control.
- Auto-play modes for crawls, sequences, and looping.
- Downstream keying and fill+key output for external switchers.

### 5. Outputs & integration
- Primary: composite into CoreVideo Pro program and preview.
- Secondary: NDI (fill + key), SDI key/fill (Blackmagic/AJA), HDMI, browser source, OBS.
- Multiple channels when a show needs independent graphic streams.
- Template export/import.

### 6. Pro production requirements
- 4K support with proper safe areas and aspect-ratio handling.
- GPU performance headroom monitoring.
- Undo, versioning, and shared libraries.
- Precise timing locked to the media clock.

## UI surfaces

**Operator console (existing layout)**
- Right panel: Graphics / Titler playlist (categorized, thumbnails, Take, status).
- Center: program/preview canvas with live graphics composited.
- Left: scenes + Zoom participants (unchanged).
- Quick actions in the transport bar (Take Lower Third, Take Score Bug, etc.).

**Title Designer (new or docked surface)**
- Left: layers hierarchy.
- Center: canvas with selection handles and bound variable placeholders.
- Bottom: timeline with keyframe tracks.
- Right: property inspector with data-binding controls (source dropdown, variable picker).

**Data Binding panel**
- Variable bindings list with live status.
- Available data sources (Zoom Participants, Google Sheets, JSON, Clock, Spreadsheet).
- Live preview of the bound graphic.

## Implementation phases

### Phase 0 – Foundation (2–3 weeks)
- Formalize graphics layer graph and variable system in the core (extend existing overlay/lower-third path).
- Typed contracts for templates, layers, bindings, and playout commands.
- Object-following / auto-scale on text length.
- Minimal playlist UI that can Take/Hide existing lower-thirds and simple graphics.

**Exit criteria:** Operators can Take a bound lower-third that auto-sizes and updates from Zoom participant data. Direct composite works. Contracts are generated and tested.

### Phase 1 – Operator Playout + Zoom-Native Binding (4–6 weeks)
- Full playlist with multi-entry (spreadsheet-style variants).
- Live data binding for Zoom participants, clocks, and simple JSON/CSV.
- In/out transitions and live property updates without full re-render.
- Hotkeys + basic Stream Deck / OSC.
- NDI fill+key output.

**Exit criteria:** A professional Zoom show can run with a playlist of lower-thirds and simple graphics driven by participant metadata, without leaving CoreVideo Pro.

### Phase 2 – Visual Designer + Animation (6–8 weeks)
- Designer surface (layers, canvas, property inspector, timeline/keyframes).
- Text, shapes, images, grouping, relative layout.
- Preset animations + custom keyframes with easing.
- PSD/SVG import.
- Brand kit integration.

**Exit criteria:** Authors can build and save custom lower-thirds and full-screen graphics without code. Designs appear in the operator playlist.

### Phase 3 – Advanced Data & Production Features (6–8 weeks)
- Tables, crawls, and leaderboards driven by data rows.
- Google Sheets, RSS, custom REST/WebSocket, social chat adapters.
- Shot layouts / macros.
- HTML template escape hatch.
- Higher layer counts, 4K polish, performance monitoring.

**Exit criteria:** The system covers the majority of NewBlue Present/Sport use cases and adds native Zoom advantages. External key/fill output is reliable.

### Phase 4 – Polish & Extensibility (ongoing)
- Sports data modules, collaboration/shared libraries, AI assistance (speech-to-lower-third, layout suggestions), multi-channel if required.

## Timeline estimate (with Claude, Codex, and Grok)

Assumes one primary developer driving the AI tools, with real-show testing. Core and UI work can be parallelized.

| Milestone | Duration | What ships |
|-----------|----------|------------|
| MVP (Phases 0 + 1) | 6–10 weeks | Usable playlist, Zoom-native lower-thirds, transitions, NDI key/fill |
| Designer + animation (through Phase 2) | 3–5 months total | Authors can build custom packages |
| Full Broadcast-class (Phase 3+) | 5–8 months total | Crawls, tables, advanced data sources, HTML escape hatch, 4K |

## Highest-leverage first deliverables

1. Variable system + Zoom participant bindings + object following.
2. Playlist + Take/Hide + transitions in the existing console.
3. Direct GPU composite path (mostly already present) + NDI key/fill.
4. Basic designer so authors can go beyond auto lower-thirds.

## Open questions / risks

- GPU text quality and performance under high layer counts (DirectWrite/D2D vs alternatives).
- Keeping the designer/operator split clean so the core stays real-time.
- HTML template latency and keying quality if the embedded-browser path is used.
- Scope control: the escape hatch and sports modules can be deferred without blocking the core value.

## Related existing work

- Compositor already supports lower-thirds and graphics templates (proven in soak).
- OverlayDirector and Magic Scene already derive nameplates from participant data.
- Brand kits exist.
- Graphics-related issues in the backlog (#1007, #1012, etc.) should be considered when scheduling Phase 0.

---

*This document is the starting point for implementation. Update it as contracts and phases land.*
