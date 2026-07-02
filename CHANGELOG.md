# Changelog

All notable changes to CoreVideo Pro are documented here. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and versions follow
[Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

### Added

- **Overlay / lower-third / caption rasterization (Item 9)**: overlays now render
  real content instead of colored rects. A shared content-tile rasterizer
  (`OverlayTileRaster`) draws the brand band, accent bar, full-ASCII 5x7 bitmap
  text, and a deterministic image placeholder; the CPU preview blits the same
  tile the D3D11 compositor uploads, so preview and program agree by
  construction. On the Windows dev rig the tile upgrades to DirectWrite/D2D
  antialiased text (brand font family) plus a real WIC `imageUri` decode, with
  graceful fallback to the CPU tile. Tiles are cached per overlay layer and
  re-raster only when content changes — keyPhase animation stays a
  composite-time transform. Caption speaker attribution now uses the secondary
  brand accent so it reads distinctly from the accent bar.

- **GPU core-composited multiview**: the C++ core composites the whole multiview grid
  into ONE keyed-mutex DXGI shared texture (replacing per-tile swap chains and CPU
  tiles), presented by a single WinUI surface with overlay labels, red/green tally,
  audio meters, and a clock; four operator-selectable layout modes (grid,
  pgm/pvw top, pgm/pvw large, pgm/pvw side) with aspect-correct 16:9 tiles.
- True multi-layer **PREVIEW composite bus** (core + WinUI + multiviewer).
- **Phase 2 threading decouple**: audio mix/route, monitor render, BS.1770 loudness,
  encoder submit, and output-sender sync moved off the render/command threads onto a
  dedicated ~50Hz worker with a two-lock (`coreMutex`/`audioOutputMutex_`) discipline;
  empty `media-core-sync` polls no longer run a heavy tick. Render thread is
  video-only, targeting locked 60fps.
- **60fps pipeline**: zero-copy Zoom I420 ingest and a precise render-loop pacer.
- Preview/program parity fixes: per-participant color grade applied to source exports
  (brightness parity) and preview-freeze fixes when a source is in both preview and
  program.
- Native Studio **Routing tab** with an audio crosspoint gain matrix: assigned
  Inputs (1–10) plus the Zoom program mix and media as rows, program/ISO/monitor/
  stream buses as columns, and per-crosspoint on/off + gain (dB) editing, wired to
  the native core via `sync-audio-routing-matrix`.
- Real program-audio pipeline in the C++ media core: PCM routing matrix with
  program/ISO taps, WASAPI monitor output (dev-gated), a BS.1770 master loudness
  meter on the program tap, and per-bus insert dynamics (compressor + limiter).
- Recording now muxes the real composed program frame plus real program audio and
  honors the requested resolution/fps from the recording command.
- RTMP sender feeds the real program-audio tap (falling back to silence only when
  no PCM is available that tick) and resolves an H.264/AAC codec-compatibility
  matrix before encoding.

### Known gaps

- The DirectWrite/WIC overlay raster is code-complete but dev-gated and has not
  run on a Windows rig yet (the portable CPU tile is the proven path); SRT
  **output** is not yet implemented; live UVC/DeckLink/AJA frames do not yet
  reach the core. See `docs/native-production-completion-plan.md`.
- The **alpha validation pass** (`docs/alpha-plan.md` Tracks A–F) has not been
  executed: no live-Zoom proof, record/stream soak, or clean-machine packaging
  evidence yet — including the ≥10-minute audio-glitch-freedom soak for the Phase 2
  threading decouple.
- No production code signing / notarization or auto-update channel yet.

## [0.1.0] - 2026-06-15

### Added

- Native desktop architecture: **WinUI 3 (.NET 9)** shell embedding the React/Vite
  operator UI, talking to a **C++ media core** over a typed JSON-line
  command/snapshot protocol, with a Node.js mirror for in-container parity.
- CI gates: segmented vitest (unit + integration), Node media-core tests, C++
  media-core stub build with a contract-parity gate, and a Windows WinUI publish
  gate.
- Windows packaging via the WinUI shell (`npm run pack:native`,
  `npm run pack:native:msix`) with optional local dev signing.
