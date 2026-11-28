#include "graphics/GraphicsFontsDirectWrite.h"

#if defined(_WIN32) && !COREVIDEO_STUB && COREVIDEO_ENABLE_DEV_ADAPTERS && COREVIDEO_WITH_D3D11

#include <dwrite_3.h>

#include <mutex>

#include "core/BoundedAsyncLog.h"
#include "graphics/GraphicsPack.h"

namespace corevideo::graphics {
namespace {
using modules::ComPtrLite;

std::wstring widen(const std::string& value) {
  if (value.empty()) return {};
  const int length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), int(value.size()), nullptr, 0);
  if (length <= 0) return {};
  std::wstring wide(size_t(length), L'\0');
  MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), int(value.size()), wide.data(), length);
  return wide;
}

template <typename T>
ComPtrLite<T> retain(T* value) {
  ComPtrLite<T> out;
  if (value) {
    value->AddRef();
    *out.put() = value;
  }
  return out;
}

bool hasFamily(IDWriteFontCollection* collection, const std::wstring& family) {
  UINT32 index = 0;
  BOOL exists = FALSE;
  return collection && !family.empty() && SUCCEEDED(collection->FindFamilyName(family.c_str(), &index, &exists)) && exists;
}

// The private pack collection. Built on a process-lifetime shared factory and
// intentionally never released: raster workers may resolve during shutdown.
struct PackCollection {
  std::mutex mutex;
  uint64_t generation = 0;
  IDWriteFontCollection* collection = nullptr;
  size_t faces = 0;
};
PackCollection& packState() {
  static auto* state = new PackCollection;
  return *state;
}

IDWriteFontCollection* buildPackCollection(size_t& faces) {
  faces = 0;
  const auto fonts = graphicsPackFonts();
  bool any = false;
  for (const auto& font : fonts) any = any || font.present;
  if (!any) return nullptr;
  ComPtrLite<IDWriteFactory5> factory;
  if (FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory5),
                                 reinterpret_cast<IUnknown**>(factory.put()))) || !factory) {
    core::nativeLogf("graphics pack fonts: IDWriteFactory5 unavailable; pack fonts will substitute\n");
    return nullptr;
  }
  ComPtrLite<IDWriteFontSetBuilder> builder;
  ComPtrLite<IDWriteFontSetBuilder1> builder1;
  if (FAILED(factory->CreateFontSetBuilder(builder.put())) ||
      FAILED(builder->QueryInterface(__uuidof(IDWriteFontSetBuilder1), reinterpret_cast<void**>(builder1.put())))) {
    core::nativeLogf("graphics pack fonts: font set builder unavailable; pack fonts will substitute\n");
    return nullptr;
  }
  // Each face is added under the family, weight and style its pack.json
  // declares, not the names inside the file. Upstream names are not uniform
  // within one family: Barlow Semi Condensed Regular and Bold carry the
  // typographic family "Barlow" (semi-condensed stretch) while its SemiBold
  // carries "Barlow Semi Condensed", so neither DirectWrite family model put
  // the three faces in one family and every weight drew the SemiBold (slice
  // 10 parity). Declared properties make the pack manifest the single source
  // of truth, which is also how the Mac raster resolves pack faces. Stretch is
  // normal: templates have no stretch and request DWRITE_FONT_STRETCH_NORMAL.
  IDWriteFontSetBuilder* propertyBuilder = builder.get();
  for (const auto& font : fonts) {
    if (!font.present) continue;
    const auto path = widen(font.path);
    const auto family = widen(font.family);
    const auto weight = std::to_wstring(font.weight);
    ComPtrLite<IDWriteFontFile> file;
    ComPtrLite<IDWriteFontFaceReference> face;
    const DWRITE_FONT_PROPERTY properties[] = {
        {DWRITE_FONT_PROPERTY_ID_WEIGHT_STRETCH_STYLE_FAMILY_NAME, family.c_str(), L"en-us"},
        {DWRITE_FONT_PROPERTY_ID_TYPOGRAPHIC_FAMILY_NAME, family.c_str(), L"en-us"},
        {DWRITE_FONT_PROPERTY_ID_WIN32_FAMILY_NAME, family.c_str(), L"en-us"},
        {DWRITE_FONT_PROPERTY_ID_WEIGHT, weight.c_str(), L""},
        {DWRITE_FONT_PROPERTY_ID_STRETCH, L"5", L""},
        {DWRITE_FONT_PROPERTY_ID_STYLE, font.italic ? L"2" : L"0", L""},
    };
    if (path.empty() || family.empty() || FAILED(factory->CreateFontFileReference(path.c_str(), nullptr, file.put())) ||
        FAILED(factory->CreateFontFaceReference(file.get(), 0, DWRITE_FONT_SIMULATIONS_NONE, face.put())) ||
        FAILED(propertyBuilder->AddFontFaceReference(face.get(), properties, UINT32(std::size(properties))))) {
      core::nativeLogf("graphics pack fonts: could not load %s (pack %s)\n", font.path.c_str(), font.packId.c_str());
      continue;
    }
    ++faces;
  }
  ComPtrLite<IDWriteFontSet> set;
  ComPtrLite<IDWriteFontCollection1> collection;
  if (faces && SUCCEEDED(builder->CreateFontSet(set.put()))) {
    // Typographic families (name ID 16): a static per-weight file such as
    // BarlowSemiCondensed-SemiBold.ttf is WWS family "Barlow Semi Condensed
    // SemiBold", so the WWS model (IDWriteFactory5) would split one pack family
    // across names by weight. Grouped typographically, the family is the name
    // pack.json and templates use, and weight/style match within it.
    ComPtrLite<IDWriteFactory6> factory6;
    ComPtrLite<IDWriteFontCollection2> typographic;
    if (SUCCEEDED(factory->QueryInterface(__uuidof(IDWriteFactory6), reinterpret_cast<void**>(factory6.put()))) &&
        SUCCEEDED(factory6->CreateFontCollectionFromFontSet(set.get(), DWRITE_FONT_FAMILY_MODEL_TYPOGRAPHIC,
                                                            typographic.put())) && typographic) {
      typographic->QueryInterface(__uuidof(IDWriteFontCollection1), reinterpret_cast<void**>(collection.put()));
    }
    if (!collection) factory->CreateFontCollectionFromFontSet(set.get(), collection.put());
  }
  if (!faces || !collection) {
    faces = 0;
    return nullptr;
  }
  core::nativeLogf("graphics pack fonts: loaded %zu faces from %s\n", faces, graphicsPacksDirectory().c_str());
  IDWriteFontCollection* out = collection.get();
  out->AddRef();
  return out;
}

ComPtrLite<IDWriteFontCollection> packCollection() {
  const auto generation = graphicsPackFontsGeneration();
  auto& state = packState();
  std::lock_guard lock(state.mutex);
  if (state.generation != generation) {
    // The previous collection is leaked on purpose: a worker may still hold
    // a text format that references it. A rescan happens only in tests.
    state.collection = buildPackCollection(state.faces);
    state.generation = generation;
  }
  return retain(state.collection);
}

}  // namespace

size_t directWritePackFontFaces() {
  packCollection();
  auto& state = packState();
  std::lock_guard lock(state.mutex);
  return state.faces;
}

DirectWriteGraphicsFont resolveDirectWriteGraphicsFont(IDWriteFactory* factory, const std::string& family) {
  DirectWriteGraphicsFont out;
  if (!factory) return out;
  const auto wide = widen(family);
  ComPtrLite<IDWriteFontCollection> system;
  if (FAILED(factory->GetSystemFontCollection(system.put())) || !system) {
    out.error = "Graphics font resolution failed.";
    return out;
  }
  if (hasFamily(system.get(), wide)) {
    out.collection = std::move(system);
    out.family = wide;
    return out;
  }
  const auto declared = graphicsPackFontDeclaration(family);
  if (!declared) return out;  // not a pack font: the caller's existing rule
  if (declared->present) {
    auto pack = packCollection();
    if (hasFamily(pack.get(), wide)) {
      out.collection = std::move(pack);
      out.family = wide;
      return out;
    }
  }
  if (graphicsPackFontsStrict()) {
    out.error = graphicsPackFontMissingError(family, declared->packId);
    return out;
  }
  const auto fallback = graphicsPackFontFallback(*declared);
  const auto wideFallback = widen(fallback);
  if (!hasFamily(system.get(), wideFallback)) {
    out.error = graphicsPackFontMissingError(family, declared->packId) + "; fallback " + fallback + " is not installed";
    return out;
  }
  out.collection = std::move(system);
  out.family = wideFallback;
  out.warning = graphicsPackFontSubstitutionWarning(family, declared->packId, fallback);
  return out;
}

}  // namespace corevideo::graphics

#endif  // _WIN32 && D3D11 dev adapters
