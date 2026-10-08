#pragma once

#include "modules/Interfaces.h"

namespace corevideo::modules {

// One translation for every production GPU path. Preserve the historical
// axis scale; named looks add small offsets in shader units. These are the
// application's built-in looks, not .cube LUT support.
struct ColorGradeParams {
  float exposure, contrast, saturation, temperature;
};

inline ColorGradeParams colorGradeParams(const CompositorColorGrade& grade) {
  ColorGradeParams result{grade.exposure * .1f, grade.contrast * .1f,
                         grade.saturation * .1f, grade.temperature * .1f};
  if (grade.lut == "neutral") {
    result.contrast += .02f;
  } else if (grade.lut == "warm-film") {
    result.contrast += .05f;
    result.saturation += .05f;
    result.temperature += .16f;
  } else if (grade.lut == "cool-broadcast") {
    result.contrast += .04f;
    result.temperature -= .14f;
  } else if (grade.lut == "punch") {
    result.contrast += .12f;
    result.saturation += .16f;
  }
  return result;
}

inline bool colorGradeIsIdentity(const CompositorColorGrade& grade) {
  const auto p = colorGradeParams(grade);
  return p.exposure == 0 && p.contrast == 0 && p.saturation == 0 && p.temperature == 0;
}

inline bool colorGradesEqual(const CompositorColorGrade& a, const CompositorColorGrade& b) {
  const auto x = colorGradeParams(a), y = colorGradeParams(b);
  return x.exposure == y.exposure && x.contrast == y.contrast &&
         x.saturation == y.saturation && x.temperature == y.temperature;
}

}  // namespace corevideo::modules
