# Changelog

All notable changes to CoreVideo Pro are documented here. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and versions follow
[Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

### Added

- Native Studio **Routing tab** with a Dante-style audio gain matrix: assigned
  Inputs (1–10) plus the Zoom program mix and media as rows, program/ISO/monitor/
  stream buses as columns, and per-crosspoint on/off + gain (dB) editing. Edits
  local production state; native command wiring (`sync-audio-routing-matrix`) and
  the video routing matrix follow per `docs/routing-ux-spec.md`.
- Lane C / C4: `electron-updater` integration with GitHub Releases feed, signed
  delta updates, and release automation (`npm run bump:version`, Release workflow).

## [0.1.0] - 2026-06-15

### Added

- Electron desktop shell with typed renderer ↔ main ↔ media-core boundary.
- Lane C CI gates: segmented vitest, native stub + contract parity, Playwright–Electron smoke.
- Desktop packaging with branded NSIS/DMG installers and optional code signing / notarization.