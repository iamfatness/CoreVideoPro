#pragma once

// Core Text font resolution and text measurement for native graphics on macOS
// (Metal graphics backend G2). ONE place decides which face a requested family
// becomes, and ONE function lays out lines, so GraphicsTemplate's measurement
// and CoreTextOverlayRaster's drawing cannot disagree.
//
// Font policy (owner decision 2026-11-21): resolution order is (1) the family
// installed in the system or process-registered collection (bundled faces and
// every pack font, GraphicsPack.h), (2) the fixed substitution table (Segoe UI
// -> bundled Selawik; Bahnschrift, Cambria, Consolas -> system families),
// (3) a pack-declared family whose file is missing: its fallback with a
// warning, unless strict pack-font mode, (4) unavailable. A TABLE substitution warns and
// never fails, strict mode included. Core Text's own automatic fallback is never
// used for the primary family, so a document always resolves the same way.
//
// The bundled faces are registered lazily, once per process, with process
// scope, from graphicsFontsDirectory(): COREVIDEO_GRAPHICS_FONTS_DIR if set,
// else GraphicsFonts/ beside the executable (Contents/Resources/GraphicsFonts
// in the .app, <build>/GraphicsFonts in a build). Provenance and checksums:
// native/resources/graphics-fonts/SOURCE.md.
//
// Plain C++ over the Core Text C API; Apple only.

#if defined(__APPLE__)

#include <CoreText/CoreText.h>

#include <cstddef>
#include <string>
#include <vector>

namespace corevideo::graphics {

struct ResolvedFontFamily {
  std::string family;        // the family to draw with
  bool available = false;    // the requested family or its table substitute is installed
  bool substituted = false;  // came from the substitution table (warn, never fail)
  int weight = 0;            // non-zero: the table pins the weight ("Segoe UI Semibold" -> 600)
};

ResolvedFontFamily resolveGraphicsFontFamily(const std::string& requested);
// A pack font (GraphicsPack.h) that is missing words the warning with its pack.
std::string graphicsFontSubstitutionWarning(const std::string& requested, const std::string& resolved);
// The preparation failure for an unavailable family: "Requested graphics font
// is unavailable: <family>", or graphicsPackFontMissingError for a missing pack
// font in strict pack-font mode.
std::string graphicsFontUnavailableMessage(const std::string& requested);
std::string graphicsFontsDirectory();
size_t registeredBundledGraphicsFonts();
// "graphics fonts: registered N faces from <dir>", or "graphics fonts: none
// found at <dir>; Windows families in the substitution table will fail".
// Registers the bundled faces first (once per process) and names the directory
// that registration used; the same line is logged once at registration.
std::string bundledGraphicsFontsSummary();

// +1 reference. Nearest real face of `family` to the CSS-style `weight`
// (400 regular, 500 medium, 600 semibold, 700 bold); Core Text never
// synthesizes bold. `italic` takes the family's italic face of that weight
// (CTFontCreateCopyWithSymbolicTraits); a family with none stays upright
// (DirectWrite would synthesize an oblique: packs ship their italic faces).
CTFontRef createGraphicsFont(const std::string& family, int weight, double size, bool italic = false);

struct GraphicsTextLine {
  CTLineRef line = nullptr;  // owned by GraphicsTextLayout
  double width = 0;          // typographic width including trailing whitespace
  double trailingWhitespace = 0;
};

// Lines of `text` set in `font`. With `wrap`, lines break at `wrapWidth`
// (CTTypesetterSuggestLineBreak); otherwise one line. `maxLines` > 0 keeps at
// most that many lines; when text remains after the last kept line, that line
// is rebuilt from the remaining text and truncated at the end with "…" to
// `truncateWidth` (also applied to a single unwrapped line wider than it).
// `kern` > or < 0 adds that many points after every character
// (kCTKernAttributeName; DirectWrite's trailing character spacing). The
// ellipsis token is never spaced, as DirectWrite's trimming sign is not.
class GraphicsTextLayout {
 public:
  GraphicsTextLayout(const std::string& text, CTFontRef font, CGColorRef color, double wrapWidth, bool wrap,
                     int maxLines, double truncateWidth, double kern = 0);
  ~GraphicsTextLayout();
  GraphicsTextLayout(const GraphicsTextLayout&) = delete;
  GraphicsTextLayout& operator=(const GraphicsTextLayout&) = delete;
  const std::vector<GraphicsTextLine>& lines() const { return lines_; }
  bool truncated() const { return truncated_; }
  unsigned totalLines() const { return totalLines_; }  // before maxLines
  double widestLine() const;
 private:
  std::vector<GraphicsTextLine> lines_;
  bool truncated_ = false;
  unsigned totalLines_ = 0;
};

struct GraphicsTextSize { double width = 0; unsigned lines = 1; };
// The measurement GraphicsTemplate's layout uses on a Metal build: the resolved
// family, the same line breaking as the raster, width including trailing
// whitespace (DirectWrite's widthIncludingTrailingWhitespace).
GraphicsTextSize measureGraphicsText(const std::string& text, const std::string& family, double size, int weight,
                                     double wrapWidth, bool wrap, double kern = 0, bool italic = false);

}  // namespace corevideo::graphics

#endif  // __APPLE__
