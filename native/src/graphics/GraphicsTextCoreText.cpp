#include "graphics/GraphicsTextCoreText.h"
#include "core/BoundedAsyncLog.h"

#if defined(__APPLE__)

#include <dirent.h>
#include <mach-o/dyld.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <climits>
#include <cstdlib>
#include <map>
#include <mutex>
#include <string_view>

namespace corevideo::graphics {
namespace {

// The owner's table (2026-11-21). Reasons are recorded in
// docs/superpowers/plans/2026-11-21-mac-metal-graphics-g2.md.
struct FontSubstitution {
  std::string_view requested;
  std::string_view macFamily;
  int weight;  // 0: keep the primitive's weight
};
constexpr std::array<FontSubstitution, 5> kFontSubstitutions{{
    {"Segoe UI", "Selawik", 0},
    {"Segoe UI Semibold", "Selawik", 600},
    {"Bahnschrift", "Avenir Next Condensed", 0},
    {"Cambria", "Georgia", 0},
    {"Consolas", "Menlo", 0},
}};

// The family an unavailable request draws with when it is allowed to draw at
// all (legacy, non-strict): the fallback the shipped Mac raster already used.
constexpr const char* kFallbackFontFamily = "Helvetica Neue";

bool equalsIgnoringCase(std::string_view a, std::string_view b) {
  return a.size() == b.size() &&
         std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) {
           return std::tolower(static_cast<unsigned char>(x)) == std::tolower(static_cast<unsigned char>(y));
         });
}

CFStringRef createCfString(const std::string& text) {
  return CFStringCreateWithBytes(kCFAllocatorDefault, reinterpret_cast<const UInt8*>(text.data()),
                                 CFIndex(text.size()), kCFStringEncodingUTF8, false);
}

std::once_flag gRegisterOnce;
size_t gRegisteredFaces = 0;
std::string gRegisteredDirectory;

std::string summaryLine() {
  return gRegisteredFaces
      ? "graphics fonts: registered " + std::to_string(gRegisteredFaces) + " faces from " + gRegisteredDirectory
      : "graphics fonts: none found at " + gRegisteredDirectory + "; Windows families in the substitution table will fail";
}

void registerBundledFonts() {
  const auto directory = graphicsFontsDirectory();
  gRegisteredDirectory = directory;
  struct LogOnExit {
    ~LogOnExit() { core::nativeLogf("%s\n", summaryLine().c_str()); }
  } log;
  DIR* dir = opendir(directory.c_str());
  if (!dir) return;
  while (const dirent* entry = readdir(dir)) {
    const std::string name = entry->d_name;
    const auto dot = name.rfind('.');
    if (dot == std::string::npos) continue;
    std::string extension = name.substr(dot + 1);
    for (auto& c : extension) c = char(std::tolower(static_cast<unsigned char>(c)));
    if (extension != "ttf" && extension != "otf") continue;
    const std::string path = directory + "/" + name;
    CFURLRef url = CFURLCreateFromFileSystemRepresentation(
        kCFAllocatorDefault, reinterpret_cast<const UInt8*>(path.c_str()), CFIndex(path.size()), false);
    if (!url) continue;
    CFErrorRef error = nullptr;
    const bool registered = CTFontManagerRegisterFontsForURL(url, kCTFontManagerScopeProcess, &error);
    if (registered) ++gRegisteredFaces;
    else if (error) {
      if (CFErrorGetCode(error) == kCTFontManagerErrorAlreadyRegistered) ++gRegisteredFaces;
      CFRelease(error);
    }
    CFRelease(url);
  }
  closedir(dir);
}

void ensureRegistered() { std::call_once(gRegisterOnce, registerBundledFonts); }

bool familyInstalledUncached(const std::string& family) {
  CFStringRef name = createCfString(family);
  if (!name) return false;
  const void* keys[] = {kCTFontFamilyNameAttribute};
  const void* values[] = {name};
  CFDictionaryRef attributes = CFDictionaryCreate(kCFAllocatorDefault, keys, values, 1,
                                                  &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
  CTFontDescriptorRef descriptor = CTFontDescriptorCreateWithAttributes(attributes);
  const void* mandatoryKeys[] = {kCTFontFamilyNameAttribute};
  CFSetRef mandatory = CFSetCreate(kCFAllocatorDefault, mandatoryKeys, 1, &kCFTypeSetCallBacks);
  CTFontDescriptorRef match = CTFontDescriptorCreateMatchingFontDescriptor(descriptor, mandatory);
  const bool installed = match != nullptr;
  if (match) CFRelease(match);
  CFRelease(mandatory);
  CFRelease(descriptor);
  CFRelease(attributes);
  CFRelease(name);
  return installed;
}

// Bounded memo: resolution runs once per text primitive on the preparation
// worker and per measurement during layout.
bool familyInstalled(const std::string& family) {
  if (family.empty() || family.size() > 256) return false;
  ensureRegistered();
  static std::mutex mutex;
  static std::map<std::string, bool> known;
  {
    std::lock_guard lock(mutex);
    if (const auto found = known.find(family); found != known.end()) return found->second;
  }
  const bool installed = familyInstalledUncached(family);
  std::lock_guard lock(mutex);
  if (known.size() >= 512) known.clear();
  known.emplace(family, installed);
  return installed;
}

CGFloat weightTrait(int weight) {
  // CSS weight -> Core Text weight trait (NSFontWeight values), linear between.
  constexpr std::array<std::pair<int, double>, 9> table{{
      {100, -0.8}, {200, -0.6}, {300, -0.4}, {400, 0.0}, {500, 0.23}, {600, 0.3}, {700, 0.4}, {800, 0.56}, {900, 0.62}}};
  weight = std::clamp(weight, 100, 900);
  for (size_t i = 1; i < table.size(); ++i) {
    if (weight <= table[i].first) {
      const double t = double(weight - table[i - 1].first) / double(table[i].first - table[i - 1].first);
      return CGFloat(table[i - 1].second + t * (table[i].second - table[i - 1].second));
    }
  }
  return CGFloat(table.back().second);
}

CFAttributedStringRef createAttributed(const std::string& text, CTFontRef font, CGColorRef color, double kern = 0) {
  CFStringRef string = createCfString(text);
  if (!string) {  // invalid UTF-8: lossy ASCII copy rather than nothing
    std::string ascii = text;
    for (auto& ch : ascii) if (static_cast<unsigned char>(ch) > 0x7f) ch = '?';
    string = createCfString(ascii);
  }
  if (!string) return nullptr;
  const CGFloat kernValue = CGFloat(kern);
  CFNumberRef kernNumber = kern != 0 ? CFNumberCreate(kCFAllocatorDefault, kCFNumberCGFloatType, &kernValue) : nullptr;
  const void* keys[3] = {kCTFontAttributeName, color ? kCTForegroundColorAttributeName : kCTForegroundColorFromContextAttributeName,
                         kCTKernAttributeName};
  const void* values[3] = {font, color ? static_cast<const void*>(color) : static_cast<const void*>(kCFBooleanTrue), kernNumber};
  CFDictionaryRef attributes = CFDictionaryCreate(kCFAllocatorDefault, keys, values, kernNumber ? 3 : 2,
                                                  &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
  CFAttributedStringRef attributed = CFAttributedStringCreate(kCFAllocatorDefault, string, attributes);
  CFRelease(attributes);
  if (kernNumber) CFRelease(kernNumber);
  CFRelease(string);
  return attributed;
}

GraphicsTextLine measured(CTLineRef line) {
  GraphicsTextLine out;
  out.line = line;
  out.width = CTLineGetTypographicBounds(line, nullptr, nullptr, nullptr);
  out.trailingWhitespace = CTLineGetTrailingWhitespaceWidth(line);
  return out;
}

CTLineRef truncate(CTLineRef line, double width, CTFontRef font, CGColorRef color) {
  CFAttributedStringRef ellipsis = createAttributed("\xE2\x80\xA6", font, color);
  CTLineRef token = ellipsis ? CTLineCreateWithAttributedString(ellipsis) : nullptr;
  CTLineRef truncated = CTLineCreateTruncatedLine(line, std::max(0.0, width), kCTLineTruncationEnd, token);
  if (token) CFRelease(token);
  if (ellipsis) CFRelease(ellipsis);
  return truncated;  // null when not even the ellipsis fits
}

}  // namespace

std::string graphicsFontsDirectory() {
  if (const char* override = std::getenv("COREVIDEO_GRAPHICS_FONTS_DIR"); override && *override) return override;
  char raw[PATH_MAX] = {};
  uint32_t size = sizeof(raw);
  if (_NSGetExecutablePath(raw, &size) != 0) return "GraphicsFonts";
  char resolved[PATH_MAX] = {};
  const std::string executable = realpath(raw, resolved) ? resolved : raw;
  const auto slash = executable.rfind('/');
  return (slash == std::string::npos ? std::string(".") : executable.substr(0, slash)) + "/GraphicsFonts";
}

size_t registeredBundledGraphicsFonts() {
  ensureRegistered();
  return gRegisteredFaces;
}

std::string bundledGraphicsFontsSummary() {
  ensureRegistered();
  return summaryLine();
}

ResolvedFontFamily resolveGraphicsFontFamily(const std::string& requested) {
  if (familyInstalled(requested)) return {requested, true, false, 0};
  for (const auto& row : kFontSubstitutions) {
    if (equalsIgnoringCase(row.requested, requested) && familyInstalled(std::string(row.macFamily)))
      return {std::string(row.macFamily), true, true, row.weight};
  }
  return {kFallbackFontFamily, false, false, 0};
}

std::string graphicsFontSubstitutionWarning(const std::string& requested, const std::string& resolved) {
  return "Font " + requested + " is not installed on this Mac; using " + resolved + ".";
}

CTFontRef createGraphicsFont(const std::string& family, int weight, double size, bool italic) {
  ensureRegistered();
  CFStringRef name = createCfString(family.empty() ? std::string(kFallbackFontFamily) : family);
  if (!name) name = createCfString(kFallbackFontFamily);
  const CGFloat trait = weightTrait(weight);
  CFNumberRef weightNumber = CFNumberCreate(kCFAllocatorDefault, kCFNumberCGFloatType, &trait);
  const void* traitKeys[] = {kCTFontWeightTrait};
  const void* traitValues[] = {weightNumber};
  CFDictionaryRef traits = CFDictionaryCreate(kCFAllocatorDefault, traitKeys, traitValues, 1,
                                              &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
  const void* keys[] = {kCTFontFamilyNameAttribute, kCTFontTraitsAttribute};
  const void* values[] = {name, traits};
  CFDictionaryRef attributes = CFDictionaryCreate(kCFAllocatorDefault, keys, values, 2,
                                                  &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
  CTFontDescriptorRef descriptor = CTFontDescriptorCreateWithAttributes(attributes);
  const void* mandatoryKeys[] = {kCTFontFamilyNameAttribute};
  CFSetRef mandatory = CFSetCreate(kCFAllocatorDefault, mandatoryKeys, 1, &kCFTypeSetCallBacks);
  CTFontDescriptorRef match = CTFontDescriptorCreateMatchingFontDescriptor(descriptor, mandatory);
  CTFontRef font = CTFontCreateWithFontDescriptor(match ? match : descriptor, CGFloat(size), nullptr);
  if (match) CFRelease(match);
  CFRelease(mandatory);
  CFRelease(descriptor);
  CFRelease(attributes);
  CFRelease(traits);
  CFRelease(weightNumber);
  CFRelease(name);
  if (italic && font) {
    CTFontRef slanted = CTFontCreateCopyWithSymbolicTraits(font, 0, nullptr, kCTFontItalicTrait, kCTFontItalicTrait);
    if (slanted) {
      CFRelease(font);
      font = slanted;
    }
  }
  return font;
}

GraphicsTextLayout::GraphicsTextLayout(const std::string& text, CTFontRef font, CGColorRef color, double wrapWidth,
                                       bool wrap, int maxLines, double truncateWidth, double kern) {
  if (text.empty() || !font) return;
  CFAttributedStringRef attributed = createAttributed(text, font, color, kern);
  if (!attributed) return;
  if (!wrap) {
    CTLineRef line = CTLineCreateWithAttributedString(attributed);
    totalLines_ = 1;
    if (line) {
      auto entry = measured(line);
      if (truncateWidth > 0 && entry.width - entry.trailingWhitespace > truncateWidth) {
        CTLineRef cut = truncate(line, truncateWidth, font, color);
        CFRelease(line);
        truncated_ = true;
        if (cut) lines_.push_back(measured(cut));
      } else {
        lines_.push_back(entry);
      }
    }
    CFRelease(attributed);
    return;
  }
  CTTypesetterRef typesetter = CTTypesetterCreateWithAttributedString(attributed);
  const CFIndex length = CFAttributedStringGetLength(attributed);
  CFIndex start = 0;
  while (typesetter && start < length) {
    CFIndex count = CTTypesetterSuggestLineBreak(typesetter, start, std::max(1.0, wrapWidth));
    if (count <= 0) count = 1;
    ++totalLines_;
    const bool keep = maxLines <= 0 || int(lines_.size()) < maxLines;
    if (keep) {
      const bool last = maxLines > 0 && int(lines_.size()) == maxLines - 1;
      if (last && start + count < length) {
        // Text remains after the last visible line: rebuild it from the
        // remaining range and end it with an ellipsis (spec 5.6).
        CTLineRef rest = CTTypesetterCreateLine(typesetter, CFRangeMake(start, length - start));
        CTLineRef cut = rest ? truncate(rest, truncateWidth > 0 ? truncateWidth : wrapWidth, font, color) : nullptr;
        if (rest) CFRelease(rest);
        truncated_ = true;
        if (cut) lines_.push_back(measured(cut));
      } else if (CTLineRef line = CTTypesetterCreateLine(typesetter, CFRangeMake(start, count))) {
        lines_.push_back(measured(line));
      }
    }
    start += count;
  }
  if (typesetter) CFRelease(typesetter);
  CFRelease(attributed);
}

GraphicsTextLayout::~GraphicsTextLayout() {
  for (auto& line : lines_) if (line.line) CFRelease(line.line);
}

double GraphicsTextLayout::widestLine() const {
  double widest = 0;
  for (const auto& line : lines_) widest = std::max(widest, line.width);
  return widest;
}

GraphicsTextSize measureGraphicsText(const std::string& text, const std::string& family, double size, int weight,
                                     double wrapWidth, bool wrap, double kern, bool italic) {
  if (text.empty()) return {0, 1};
  const auto resolved = resolveGraphicsFontFamily(family);
  CTFontRef font = createGraphicsFont(resolved.family, resolved.weight ? resolved.weight : weight, size, italic);
  if (!font) return {0, 1};
  GraphicsTextLayout layout(text, font, nullptr, wrapWidth, wrap, 0, 0, kern);
  CFRelease(font);
  return {layout.widestLine(), std::max(1u, layout.totalLines())};
}

}  // namespace corevideo::graphics

#endif  // __APPLE__
