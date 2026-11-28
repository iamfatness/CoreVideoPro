# Built-in graphics packs: font provenance

Each folder here is a graphics style pack (`docs/reference/graphics-packs.md`). CMake copies this
directory to `packs/` beside every build's executables; `scripts/package-alpha.ps1` stages it beside
`corevideo-native.exe` in the Windows install, and `scripts/make-macos-shell-bundle.sh` into
`CoreVideo Pro.app/Contents/Resources/packs/`. Every font is SIL Open Font License 1.1 and ships
unmodified, with its licence text as `fonts/OFL.txt` in its pack.

`GraphicsPack.ShippedPackFontsMatchTheirPinnedChecksums` fails if a file changes.

## news-signal

### Barlow Semi Condensed SemiBold

- Source: https://raw.githubusercontent.com/google/fonts/main/ofl/barlowsemicondensed/BarlowSemiCondensed-SemiBold.ttf
  (google/fonts `main` at 5026800714d07231937d92eb4cceabf06e05d74e, downloaded 2026-11-28; last change to
  the file upstream: 89f5431ff0db41bd2fe3f7ba21a723a01622428b, 2018-12-05)
- Licence: SIL Open Font License 1.1, Copyright 2017 The Barlow Project Authors
  (https://github.com/jpt/barlow). Text in `news-signal/fonts/OFL.txt`, verbatim from
  https://raw.githubusercontent.com/google/fonts/main/ofl/barlowsemicondensed/OFL.txt

| file | face | SHA-256 |
|---|---|---|
| news-signal/fonts/BarlowSemiCondensed-SemiBold.ttf | Barlow Semi Condensed SemiBold (600) | bd299f4bc5b44d30be8d42e9bad3a5df7d66af1cd55d0ed72a8b8916360a1424 |
| news-signal/fonts/OFL.txt | licence | 186d750eb496a4c17a76385f82be6aea2ac1cf2de074a811d63786cf374ea73f |
