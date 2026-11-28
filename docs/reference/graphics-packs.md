# Graphics style packs and bundled pack fonts

Read before touching a pack (`native/resources/graphics-packs/`), the pack lint, pack install, or how
graphics text picks a font. Spec: `graphics-styles-color-lower-thirds-spec.md` 6.1 P8, 6.2, 6.3, 7
(font rule) and 8 row 7. Code: `native/src/graphics/GraphicsPack.{h,cpp}` (reader, lint, font
registry, portable), `GraphicsFontsDirectWrite.{h,cpp}` (Windows), `GraphicsTextCoreText.cpp` (Mac).

## The format

```
packs/<id>/
  pack.json    {id, name, genre, version, palettes:[...], fonts:[...], templates:[...], metadata?}
  palettes/    <name>.json  {"brand.primary": "#RRGGBBAA", ...}
  templates/   <name>.json  ordinary declarative template documents
  fonts/       OFL .ttf/.otf + OFL.txt
  assets/      PNG only, <= 2 MB per pack (template assets: assets/<sha256>.png)
```

- `id` = folder name: lowercase letters, digits, hyphens, leading letter, at most 64.
- `fonts[]` entries: `{family, file: "fonts/<name>.ttf|otf", weight: 100..900 step 100, style:
  normal|italic, fallback?}`. One entry per face. `fallback` (default `Segoe UI`) is what a missing
  file draws with.
- Palette tokens: the seven slice-1 roles are **required** (`brand.primary`, `brand.secondary`,
  `brand.accent`, `brand.onPrimary`, `brand.onSecondary`, `brand.onAccent`, `brand.surface`);
  `brand.teamA`/`brand.teamB` and the older colour tokens (`brand.onPrimaryMuted`, `brand.background`,
  `brand.foreground`) are optional. Values are exactly `#RRGGBBAA`.
- Templates may only use template tokens that exist on `main` today. Until slice 1 (#1271) adds
  `brand.onSecondary`/`onAccent`/`surface` to the template token set, a pack template cannot reference
  them (the loader refuses the template). The News anchor therefore uses `brand.onPrimary` for its
  title; switch to `brand.onSecondary` when slice 1 merges.
- Packs are core data: no shell code. Built-in packs ride beside the core in `packs/`.

## The pack lint (CI)

`lintGraphicsPack(dir)` returns every problem at once; `GraphicsPack.ShippedPacksPassTheLint` runs it
over every folder in `native/resources/graphics-packs/` in every native test build (Windows, and the
Linux and macOS stub jobs in `ci.yml`). It rejects:

- a raw hex colour in a template layer unless that layer says `"fixed": true` (a white LIVE dot, a
  texture tint). Content keys (`text`, `items`, `rows`, ids and enums) are never treated as colours.
  `"fixed"` is accepted by the template loader on every node and ignored when drawing;
- a template the declarative loader refuses: 64 layers, 32 fields, motion <= 3000 ms and everything
  else the loader checks; plus the lint's own 64-layer / 32-field count so the message is plain;
- a literal `font` that is not one of the pack's families, and a text layer whose weight/style the
  pack does not ship (**packs ship every weight they use**: Windows would synthesize bold, the Mac
  would not; spec 7 known differences). Font tokens (`{"token":"brand.fontBody"}`) are allowed;
- a declared font file missing or not TrueType/OpenType, an undeclared file in `fonts/`, a missing
  `fonts/OFL.txt`, more than 8 MB of fonts;
- palettes with unknown tokens, a missing required token, or a value that is not `#RRGGBBAA`; an
  unlisted file in `palettes/` or `templates/`;
- an asset that is not a PNG (extension and signature), or more than 2 MB of assets in one pack.

## Install

`graphics-template-install` takes **either** `{manifest, packageRoot?}` (one template, unchanged)
**or** `{packRoot}` (a pack folder), never both (`graphics-pack-root-invalid`). A pack install is one
queued job: the worker runs the full lint, then validates every listed template with the pack root as
package root, and publishes all of them or none. Failure reasons: `graphics-pack-invalid` (lint; the
install row's `detail` names the first problem) or the loader/registry reason of the first failing
template (`detail`: `template N: ...`). Install rows of pack jobs carry `packRoot`. Library limits are
unchanged: 16 imported versions, 256 KiB of documents.

**Built-in packs enter the catalog as built-ins (slice 11).** The production core calls
`MediaCore::loadBuiltInGraphicsPacks(graphicsPacksDirectory())` once at startup
(`GraphicsTemplateRegistry::loadBuiltInPacks`): one worker job, never blocking startup, lints every
pack folder and publishes each passing pack's templates with `pack: {id, name, genre}` and
`builtIn: true`. They do not count toward the 16 versions or 256 KiB, cannot be removed
(`template-built-in`) and never appear in `templateInstalls`. A pack that fails the lint or the
loader publishes nothing and is listed in the graphics snapshot's `templatePacks` as `invalid` with
its first problem; the others load. An identical operator import of the same version becomes the
built-in one. Tests and other `MediaCore` hosts load no packs unless they call it, so their catalogs
stay the curated set. The Builder (`graphics-playout.md`, "Style packs in the Builder") is the
shell surface; the gateway still does not expose `packRoot`.

Why built-ins and not install-on-pick (considered in slice 11): installing a pack template only
when the operator picks its card, through the 16-version library and `GraphicsDesignEviction`, would
(a) leave the style strip without live thumbnails for packs not yet installed. The spec requires
every card to be core-rendered in the show palette, and thumbnails render only from catalog
definitions. (b) It would let a saved show row's pack template be evicted, so a row could stop
resolving after a restart or after other imports. (c) It would spend operator library slots on data
that ships with the app. Built-in pack templates are immutable app data, exactly like the curated
looks, so they take the curated looks' path. Nothing about the imported limits changed: 16 versions
and 256 KiB still apply to operator imports and designs, and
`GraphicsBuiltInPacks.BuiltInPacksNeverSpendTheImportedLibrarysSlots` proves the 11th import fits
and the 12th is refused with packs loaded. Preparation admission (`GraphicsResourceBudget`) is
untouched: a catalog definition prepares nothing until an instance or preview uses it. The cost is
memory for the definitions (documents plus at most 2 MB of PNG assets per pack) and, once the
Builder asks, one 320×180 thumbnail per template.

## Fonts (P8)

The pack font registry scans `graphicsPacksDirectory()`: `COREVIDEO_GRAPHICS_PACKS_DIR` if set, else
`packs/` beside the executable. Every readable pack's `fonts[]` is listed, with `present` false for a
missing or non-font file.

- **Windows** (`resolveDirectWriteGraphicsFont`, used by both `GraphicsTemplate` measurement and
  `CompositorOverlayRaster::drawTemplate`, so layout measures the face that draws):
  1. the system font collection, exactly as before packs (every existing graphic and reference row
     is unchanged);
  2. the **private pack collection**: an `IDWriteFactory5` font set built with
     `IDWriteFontSetBuilder1::AddFontFile` over every present pack font, turned into a collection
     with `CreateFontCollectionFromFontSet`. Nothing is installed system-wide. Built once per
     registry generation on a process-lifetime factory and never released (workers may resolve
     during shutdown);
  3. a pack-declared family whose file is missing or did not load: its `fallback` from the system
     collection plus the warning `Font <family> is missing from pack <id>; using <fallback>.` The
     graphic is **ready**, strict or not: never a failure on air;
  4. anything else: the existing rule (strict graphics fail "Requested graphics font is unavailable",
     legacy lower thirds fall back to Segoe UI and warn).
- **Mac** (`GraphicsTextCoreText.cpp`): pack font files are registered with
  `CTFontManagerRegisterFontsForURL` at process scope, like the bundled Selawik faces, once per file,
  re-checked when the registry generation changes. A missing pack font resolves its `fallback`
  through the substitution table (Segoe UI becomes Selawik) and warns with the same pack wording.
  The Mac shell names the app's `Contents/Resources/packs` in `COREVIDEO_GRAPHICS_PACKS_DIR`
  (`CoreLaunchEnvironment.extras`), because the pkg-installed core runs outside the app.
- **Strict pack-font mode** (`COREVIDEO_GRAPHICS_STRICT_PACK_FONTS=1`, or
  `setGraphicsPackFontsStrictForTesting`) turns step 3 into a failure:
  `Pack font is missing: <family> (pack <id>)`. Tests only.

## Where packs are staged

- Build: CMake copies `native/resources/graphics-packs/` to `<build>/packs/` at configure
  (`CONFIGURE_DEPENDS`; re-run configure after adding a file). `build-native-dev.ps1` also stages it
  into `build-dev/Release/packs`.
- Windows package: `scripts/package-alpha.ps1` copies it from the repo to `<app>/packs/` beside
  `corevideo-native.exe` and fails if `news-signal/pack.json` is missing.
- Mac: `scripts/make-macos-shell-bundle.sh` copies it to `CoreVideo Pro.app/Contents/Resources/packs/`.

## Adding a pack or a font

1. Folder `native/resources/graphics-packs/<id>/` with the layout above; fonts from an OFL source
   with the licence text as `fonts/OFL.txt`. Prefer static per-weight TTFs over variable fonts.
2. Record source URL, upstream commit, licence and SHA-256 per file in
   `native/resources/graphics-packs/SOURCE.md` and pin the hashes in
   `GraphicsPack.ShippedPackFontsMatchTheirPinnedChecksums`. `.gitattributes` keeps `fonts/**`
   byte-exact (`-text`).
3. Each template gets a parity fixture in `native/tests/fixtures/graphics-parity/index.json`
   (`manifest:../../../resources/graphics-packs/<id>/templates/<t>.json`, brand = the default
   palette's colours), a Windows reference row (`COREVIDEO_WRITE_GRAPHICS_REFERENCE=<repo root>`
   with `--gtest_filter=GraphicsRasterReference.WriteWindowsReference`) and, on the owner's Mac, a
   Mac golden (`COREVIDEO_WRITE_MAC_GOLDENS=1`). `GraphicsRasterParity.MacMatchesWindowsReference`
   on Mac CI is blocking; never loosen a tolerance.

## Shipped packs

| Pack | Templates | Palettes | Fonts |
|---|---|---|---|
| `news-signal` (first piece of News: Signal, slice 7) | `anchor` (name + uppercase tracked title, accent strip, fixed 2 px highlight) | `navy-red` | Barlow Semi Condensed SemiBold 600 (OFL, google/fonts) |
