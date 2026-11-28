#pragma once

// Graphics style packs (graphics styles spec 6.2, slice 7). A pack is a folder
//
//   <packs>/<id>/
//     pack.json    {id, name, genre, version, palettes[], fonts[], templates[]}
//     palettes/    <name>.json: {"brand.primary": "#RRGGBBAA", ...}
//     templates/   <name>.json: ordinary declarative template documents
//     fonts/       OFL .ttf/.otf files plus OFL.txt
//     assets/      PNG only, at most 2 MB per pack
//
// that is core data: the Windows and Mac shells get packs with no shell code.
// This file is the one portable reader, validator (the CI pack lint) and font
// registry. Platform text code asks it which families packs declare, where
// their files are and what to draw when a pack font is missing.
//
// Font rule (spec 7): a pack font whose file is missing or fails to load is
// substituted with the font's declared `fallback` (default "Segoe UI") and the
// graphic carries a warning; it never fails on air. Strict pack-font mode
// (graphicsPackFontsStrict) turns that substitution into a preparation failure
// for tests.

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace corevideo::graphics {

struct GraphicsPackFont {
  std::string family;    // the family name templates use, e.g. "Barlow Semi Condensed"
  std::string file;      // "fonts/<file>.ttf|otf", relative to the pack root
  int weight = 400;      // 100..900
  std::string style = "normal";  // normal | italic
  std::string fallback;  // drawn when the file is missing; empty = "Segoe UI"
};

struct GraphicsPackManifest {
  std::string root;  // absolute pack directory (UTF-8)
  std::string id, name, genre, version;
  std::vector<std::string> palettes;   // "palettes/<name>.json"
  std::vector<GraphicsPackFont> fonts;
  std::vector<std::string> templates;  // "templates/<name>.json"
};

// Limits a pack must stay inside (spec 6.2). Template limits are the
// declarative loader's own (64 layers, 32 fields, <=3000 ms motion).
inline constexpr uint64_t kGraphicsPackAssetBytesLimit = 2ull * 1024 * 1024;
inline constexpr size_t kGraphicsPackTemplateLimit = 16;
inline constexpr size_t kGraphicsPackPaletteLimit = 8;
inline constexpr size_t kGraphicsPackFontLimit = 16;
inline constexpr uint64_t kGraphicsPackFontBytesLimit = 8ull * 1024 * 1024;

// Palette tokens a pack palette may set. The seven slice-1 roles are required;
// team colours and the older colour tokens are optional.
const std::vector<std::string>& graphicsPackRequiredPaletteTokens();
bool graphicsPackPaletteTokenKnown(const std::string& token);

// Reads and checks pack.json only (shape, ids, relative paths). `dir` is UTF-8.
bool readGraphicsPackManifest(const std::string& dir, GraphicsPackManifest& out, std::string& error);

// The full pack lint (spec 6.2 rules). Every problem, one line each; empty
// means the pack passes. Checks: the manifest; the id matches the folder;
// each palette parses with known tokens, every required token and #RRGGBBAA
// values; each font file exists in fonts/ with a TrueType/OpenType signature
// and fonts/OFL.txt is present; each template loads through the declarative
// loader (64 layers, 32 fields, <=3000 ms motion), uses no raw hex colour
// unless its node says "fixed": true, and names only fonts the pack declares
// (font tokens resolve from the show palette and are allowed); assets/ holds
// only PNG files, 2 MB in total; no undeclared files in fonts/ or templates/.
std::vector<std::string> lintGraphicsPack(const std::string& dir);

// The template documents a pack installs, in manifest order, each with the
// package root graphics-template-install resolves assets against (the pack
// root). Runs the full lint first and refuses a pack with any problem.
bool graphicsPackInstallManifests(const std::string& dir, std::vector<std::string>& manifests, std::string& error);

// Where built-in packs live: COREVIDEO_GRAPHICS_PACKS_DIR if set, else packs/
// beside the executable (the build directory, the Windows install directory,
// CoreVideo Pro.app/Contents/Resources/packs via the Mac shell's launch env).
std::string graphicsPacksDirectory();

// One font file a pack declares, as discovered under graphicsPacksDirectory().
struct GraphicsPackFontFile {
  std::string packId, family, path, fallback;  // path is absolute UTF-8
  int weight = 400;
  bool italic = false;
  bool present = false;  // the file exists and has a font signature
};

// Every font file of every readable pack under the packs directory, scanned
// once per generation. Packs whose manifest does not read are skipped (the
// lint reports them); a declared file that is missing is listed with
// present = false so text code can substitute and warn.
std::vector<GraphicsPackFontFile> graphicsPackFonts();
// The declaration for `family` (case-insensitive), if any pack declares it.
std::optional<GraphicsPackFontFile> graphicsPackFontDeclaration(const std::string& family);
// Bumps whenever the scanned set can have changed; platform caches key on it.
uint64_t graphicsPackFontsGeneration();
// The family a missing pack font draws with (its fallback, else "Segoe UI").
std::string graphicsPackFontFallback(const GraphicsPackFontFile& font);
// "Font <family> is missing from pack <id>; using <substitute>."
std::string graphicsPackFontSubstitutionWarning(const std::string& family, const std::string& packId,
                                                const std::string& substitute);
// "Pack font is missing: <family> (pack <id>)" - strict mode's failure.
std::string graphicsPackFontMissingError(const std::string& family, const std::string& packId);

// Strict pack-font mode: a missing pack font fails preparation instead of
// substituting. On when COREVIDEO_GRAPHICS_STRICT_PACK_FONTS=1 or set here.
bool graphicsPackFontsStrict();
void setGraphicsPackFontsStrictForTesting(bool strict);
// Points discovery at `dir` (empty: back to graphicsPacksDirectory()) and
// rescans on next use. Tests only.
void setGraphicsPacksDirectoryForTesting(const std::string& dir);

}  // namespace corevideo::graphics
