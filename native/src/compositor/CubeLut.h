#pragma once
#include "modules/Interfaces.h"
#include "modules/Sha256.h"
#include <sstream>
#include <iomanip>
#include <locale>
#include <cmath>
namespace corevideo::modules {
inline std::shared_ptr<const GradeCubeLut> parseGradeCube(const std::string& text, const std::string& expectedHash) {
  if (text.size() > 2000000 || expectedHash.size() != 64) return {};
  const auto digest = hashing::sha256(reinterpret_cast<const uint8_t*>(text.data()),text.size());
  std::ostringstream hash; hash << std::hex << std::setfill('0'); for (auto b : digest) hash << std::setw(2) << int(b);
  if (hash.str() != expectedHash) return {};
  auto result = std::make_shared<GradeCubeLut>(); result->hash = expectedHash;
  bool minSeen = false, maxSeen = false;
  std::istringstream input(text); input.imbue(std::locale::classic()); std::string raw;
  while (std::getline(input,raw)) {
    const auto comment = raw.find('#'); if (comment != std::string::npos) raw.resize(comment);
    std::istringstream line(raw); line.imbue(std::locale::classic()); std::string first, extra;
    if (!(line >> first)) continue;
    if (first == "TITLE") continue;
    if (first == "LUT_3D_SIZE") {
      if (result->size || !(line >> result->size) || result->size < 2 || result->size > 33 || (line >> extra)) return {};
      continue;
    }
    if (first == "DOMAIN_MIN" || first == "DOMAIN_MAX") {
      const bool minimum = first == "DOMAIN_MIN";
      if ((minimum && minSeen) || (!minimum && maxSeen)) return {};
      auto* values = minimum ? result->domainMin : result->domainMax;
      for (int i=0;i<3;++i) if (!(line >> values[i]) || !std::isfinite(values[i]) || std::abs(values[i]) > 16) return {};
      if (line >> extra) return {}; if (minimum) minSeen = true; else maxSeen = true; continue;
    }
    if (!result->size || result->rgba.size() >= size_t(result->size*result->size*result->size*4)) return {};
    std::istringstream numbers(first + " " + std::string(std::istreambuf_iterator<char>(line),{})); numbers.imbue(std::locale::classic());
    for (int i=0;i<3;++i) { float value; if (!(numbers>>value) || !std::isfinite(value) || std::abs(value)>16) return {}; result->rgba.push_back(value); }
    if (numbers>>extra) return {}; result->rgba.push_back(1);
  }
  if (!result->size || result->rgba.size() != size_t(result->size*result->size*result->size*4)) return {};
  for (int i=0;i<3;++i) if (result->domainMax[i] <= result->domainMin[i]) return {};
  return result;
}
}
