#pragma once
#include "compositor/LowerThirdAppearance.h"
#include <set>
#include <string>
namespace corevideo::graphics {
inline compositor::LowerThirdAppearance defaultAppearance(const std::string& templateId) {
  compositor::LowerThirdAppearance a;a.enabled=true;
  a.preset=templateId=="minimal-accent" || templateId=="broadcast" ? templateId : "compact-solid";
  if(templateId=="minimal-accent") {a.backgroundOpacity=0;a.nameSize=40;a.titleSize=26;a.cornerRadius=0;}
  else if(templateId=="broadcast") {a.nameSize=52;a.titleSize=32;a.padding=24;a.width=.75;a.cornerRadius=0;a.backgroundOpacity=1;}
  return a;
}

// Show palette (graphics styles spec 4.1): the colour tokens a palette may set.
// The shell resolves row override -> show palette before it sends a revision; the
// core lays that map over the row-appearance derivation below, which stands in
// for the pack default until packs ship. Values are exactly #RRGGBBAA.
inline const std::set<std::string>& graphicsPaletteTokens() {
  static const std::set<std::string> tokens{"brand.primary", "brand.secondary", "brand.accent", "brand.onPrimary",
      "brand.onPrimaryMuted", "brand.onSecondary", "brand.onAccent", "brand.surface", "brand.teamA", "brand.teamB"};
  return tokens;
}
inline bool graphicsPaletteColor(const std::string& value) {
  if (value.size() != 9 || value[0] != '#') return false;
  for (size_t i = 1; i < value.size(); ++i) {
    const char c = value[i];
    if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'))) return false;
  }
  return true;
}
// Null is the empty palette. Anything else must be an object of known colour
// tokens to #RRGGBBAA strings.
inline bool graphicsPaletteValid(const rpc::Json& palette) {
  if (palette.isNull()) return true;
  if (!palette.isObject() || palette.asObject().size() > graphicsPaletteTokens().size()) return false;
  for (const auto& [token, value] : palette.asObject())
    if (!graphicsPaletteTokens().count(token) || !value.isString() || !graphicsPaletteColor(value.asString())) return false;
  return true;
}
// Brand token values for a declarative template. `primary` is the appearance's
// plate colour, with or without its opacity, exactly as each caller drew it
// before the palette existed. Palette values replace the derived ones.
inline rpc::Json graphicsBrand(const compositor::LowerThirdAppearance& a, const std::string& primary, bool hasLogo,
    const rpc::Json& palette = {}) {
  rpc::Json::Object brand{{"brand.primary", primary}, {"brand.onPrimary", a.nameColor},
      {"brand.onPrimaryMuted", a.titleColor}, {"brand.fontBody", a.fontFamily},
      {"brand.secondary", a.titleColor}, {"brand.accent", a.accentColor},
      {"brand.background", a.backgroundColor}, {"brand.foreground", a.nameColor}, {"brand.fontHeading", a.fontFamily},
      {"brand.logo", hasLogo ? "logo" : ""},
      // Palette-era tokens: derived from the same appearance when no palette sets them.
      {"brand.onSecondary", a.nameColor}, {"brand.onAccent", a.nameColor}, {"brand.surface", primary},
      {"brand.teamA", a.accentColor}, {"brand.teamB", a.titleColor}};
  if (palette.isObject()) {
    for (const auto& [token, value] : palette.asObject())
      if (graphicsPaletteTokens().count(token) && value.isString()) brand[token] = value.asString();
    // The two legacy aliases follow the palette too: background is the primary plate
    // opaque (it was always #RRGGBB), foreground the text on it.
    if (const auto* primaryValue = palette.get("brand.primary"); primaryValue && primaryValue->isString())
      brand["brand.background"] = primaryValue->asString().substr(0, 7);
    if (const auto* onPrimary = palette.get("brand.onPrimary"); onPrimary && onPrimary->isString())
      brand["brand.foreground"] = onPrimary->asString();
  }
  return rpc::Json(std::move(brand));
}
}
