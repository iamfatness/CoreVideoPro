# Changelog

All notable changes to CoreVideo Pro are documented here. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and versions follow
[Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

### Added

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

- Overlay / lower-third / caption **text and image rasterization** (DirectWrite/WIC)
  is unfinished; SRT **output** is not yet implemented; live UVC/DeckLink/AJA frames
  do not yet reach the core. See `docs/native-production-completion-plan.md`.
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
