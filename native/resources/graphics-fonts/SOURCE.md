# Bundled graphics fonts

Registered by the media core at runtime with process scope
(`native/src/graphics/GraphicsTextCoreText.cpp`, `CTFontManagerRegisterFontsForURL`) so a template
that asks for a Windows family can resolve to a metric-compatible face on macOS. Copied to
`GraphicsFonts/` beside the core binary: by CMake into every macOS build directory, and by
`scripts/make-macos-shell-bundle.sh` into `CoreVideo Pro.app/Contents/Resources/GraphicsFonts/`.

## Selawik 1.01 (substitute for Segoe UI)

- Source: https://github.com/microsoft/Selawik/releases/download/1.01/Selawik_Release.zip
  (release 1.01, published 2015-12-08)
- Zip SHA-256: `3f62c51e05e3b5a1e6241cf92a371f0be2ea1183aa87b30718bbd40832a8d423`
- Licence: SIL Open Font License 1.1, Reserved Font Name "Selawik". Text in `OFL.txt`, verbatim from
  https://raw.githubusercontent.com/microsoft/Selawik/1.01/LICENSE.txt
- Shipped unmodified, under the original file names. Light and Semilight are not shipped.

| file | face | SHA-256 |
|---|---|---|
| selawk.ttf | Selawik-Regular | e9d98518d8ac2817782a9a382430463a2e0793ea68350b695bb727d9a830ee1c |
| selawksb.ttf | Selawik-Semibold | 0a9e9d0549a10f24bef9b3a29e06fe6e0b5c21e7a784c503b048a307841a7783 |
| selawkb.ttf | Selawik-Bold | f0db5e174a90e0956ad7d2844bdca1d5e6da92ec65b2c04e57ba9b180668c904 |

`GraphicsFontResolver.BundledFilesMatchTheirPinnedChecksums` fails if a file changes.
