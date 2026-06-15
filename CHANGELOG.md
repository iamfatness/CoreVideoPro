# Changelog

All notable changes to CoreVideo Pro are documented here. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and versions follow
[Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

### Added

- Lane C / C4: `electron-updater` integration with GitHub Releases feed, signed
  delta updates, and release automation (`npm run bump:version`, Release workflow).

## [0.1.0] - 2026-06-15

### Added

- Electron desktop shell with typed renderer ↔ main ↔ media-core boundary.
- Lane C CI gates: segmented vitest, native stub + contract parity, Playwright–Electron smoke.
- Desktop packaging with branded NSIS/DMG installers and optional code signing / notarization.