#pragma once
#include "modules/Interfaces.h"
#include "rpc/Json.h"
#include "compositor/CubeLut.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <set>
#include <mutex>
#include <deque>

namespace corevideo::modules {
class GradeDocuments {
 public:
  static std::shared_ptr<const AdvancedGradeDocument> find(const std::string& ref) {
    std::lock_guard<std::mutex> lock(gate()); const auto it=cache().find(ref);
    return it==cache().end() ? nullptr : it->second.lock();
  }
  static void remember(const std::string& ref, std::shared_ptr<const AdvancedGradeDocument> doc) {
    std::lock_guard<std::mutex> lock(gate());
    for(auto it=cache().begin();it!=cache().end();) it=it->second.expired() ? cache().erase(it) : std::next(it);
    if(cache().size()>=256) cache().erase(cache().begin());
    cache()[ref]=doc; recent().push_back(std::move(doc)); while(recent().size()>16) recent().pop_front();
  }
 private:
  static std::mutex& gate() { static std::mutex m; return m; }
  static std::map<std::string,std::weak_ptr<const AdvancedGradeDocument>>& cache() { static std::map<std::string,std::weak_ptr<const AdvancedGradeDocument>> c; return c; }
  static std::deque<std::shared_ptr<const AdvancedGradeDocument>>& recent() { static std::deque<std::shared_ptr<const AdvancedGradeDocument>> r; return r; }
};
inline bool readAdvancedGrade(const rpc::Json& node, std::shared_ptr<const AdvancedGradeDocument>& result) {
  if (node.isNull()) { result.reset(); return true; }
  if (node.isObject() && node.get("documentRef")) { result=GradeDocuments::find(node.getString("documentRef")); return bool(result); }
  if (!node.isObject() || node.getNumber("version", -1) != 2 || node.getString("colorSpace") != "rec709-sdr") return false;
  auto doc = std::make_shared<AdvancedGradeDocument>();
  const auto number = [](const rpc::Json& n, const char* key, float fallback, double lo, double hi, float& out) {
    const auto* value = n.get(key);
    if (value && !value->isNumber()) return false;
    const auto v = n.getNumber(key, fallback);
    if (!std::isfinite(v) || v < lo || v > hi) return false;
    out = static_cast<float>(v); return true;
  };
  if (!number(node, "intensity", 1, 0, 1, doc->intensity)) return false;
  if (const auto* b = node.get("bypass")) { if (!b->isBool()) return false; doc->bypass = b->asBool(); }
  const auto* operations = node.get("operations");
  if (!operations || !operations->isArray() || operations->asArray().size() > 8) return false;
  std::set<std::string> ids;
  for (const auto& n : operations->asArray()) {
    GradeOperation o; o.id = n.getString("id"); o.kind = n.getString("kind");
    if (o.id.empty() || o.id.size() > 64 || !ids.insert(o.id).second || (o.kind != "primaries" && o.kind != "curves" && o.kind != "cube")) return false;
    if (o.kind == "cube") { o.cube = parseGradeCube(n.getString("cubeText"),n.getString("cubeSha256")); if (!o.cube) return false; }
    if (const auto* e = n.get("enabled")) { if (!e->isBool()) return false; o.enabled = e->asBool(); }
    if (!number(n,"intensity",1,0,1,o.intensity) || !number(n,"exposureStops",0,-8,8,o.exposureStops) ||
        !number(n,"contrast",1,0,4,o.contrast) || !number(n,"pivot",.5f,0,1,o.pivot) ||
        !number(n,"saturation",1,0,4,o.saturation) || !number(n,"temperature",0,-1,1,o.temperature) ||
        !number(n,"tint",0,-1,1,o.tint) || !number(n,"lift",0,-1,1,o.lift) ||
        !number(n,"gamma",1,.1,4,o.gamma) || !number(n,"gain",1,0,4,o.gain)) return false;
    const auto* curves = n.get("curves");
    if (!curves || !curves->isArray() || curves->asArray().size() != 4) return false;
    for (const auto& channel : curves->asArray()) {
      if (!channel.isArray() || channel.asArray().size() < 2 || channel.asArray().size() > 16) return false;
      std::vector<GradeCurvePoint> points;
      float previous = -1;
      for (const auto& p : channel.asArray()) {
        GradeCurvePoint point;
        if (!p.isObject() || !number(p,"x",-1,0,1,point.x) || !number(p,"y",-1,0,1,point.y) || point.x <= previous) return false;
        points.push_back(point); previous = point.x;
      }
      if (points.front().x != 0 || points.back().x != 1) return false;
      o.curves.push_back(std::move(points));
    }
    doc->operations.push_back(std::move(o));
  }
  const auto serialized = node.stringify();
  const auto digest = hashing::sha256(reinterpret_cast<const uint8_t*>(serialized.data()),serialized.size());
  std::ostringstream key; key << std::hex << std::setfill('0'); for (auto b : digest) key << std::setw(2) << int(b);
  doc->content = key.str(); GradeDocuments::remember(doc->content,doc); result = std::move(doc); return true;
}

struct AdvancedGradeConstants {
  float header[4]{}; // count, global intensity, enabled, reserved
  struct Operation { float meta[4]{}, primary[4]{}, balance[4]{}, domainMin[4]{}, domainMax[4]{}; } operations[8];
};
static_assert(sizeof(AdvancedGradeConstants) == 656, "GPU advanced-grade layout");
inline AdvancedGradeConstants advancedGradeConstants(const CompositorColorGrade& grade) {
  AdvancedGradeConstants c;
  if (!grade.advanced || grade.advanced->bypass) return c;
  c.header[0] = float(grade.advanced->operations.size()); c.header[1] = grade.advanced->intensity; c.header[2] = 1;
  for (size_t i = 0; i < grade.advanced->operations.size(); ++i) {
    const auto& o = grade.advanced->operations[i]; auto& p = c.operations[i];
    p.meta[0] = o.kind == "cube" ? 3.f : o.kind == "curves" ? 2.f : 1.f; p.meta[1] = o.enabled ? o.intensity : 0.f;
    p.primary[0] = o.exposureStops; p.primary[1] = o.contrast; p.primary[2] = o.pivot; p.primary[3] = o.saturation;
    p.balance[0] = o.temperature; p.balance[1] = o.tint; p.balance[2] = o.lift; p.balance[3] = o.gamma;
    p.meta[2] = o.gain;
    if (o.cube) { p.meta[3] = float(o.cube->size); for (int c=0;c<3;++c) { p.domainMin[c] = o.cube->domainMin[c]; p.domainMax[c] = o.cube->domainMax[c]; } }
  }
  return c;
}
inline float evaluateGradeCurve(const std::vector<GradeCurvePoint>& points, float x) {
  for (size_t i = 1; i < points.size(); ++i) if (x <= points[i].x) {
    const auto& a = points[i-1]; const auto& b = points[i];
    return a.y + (b.y-a.y) * (x-a.x) / (b.x-a.x);
  }
  return points.back().y;
}
inline std::array<float, 16 * 32 * 2> compileGradeCurves(const CompositorColorGrade& grade) {
  std::array<float, 16 * 32 * 2> points{};
  for (size_t operation=0;operation<8;++operation) for(size_t channel=0;channel<4;++channel) for(size_t i=0;i<16;++i) {
    GradeCurvePoint point{1,1};
    if(grade.advanced && operation<grade.advanced->operations.size()) {
      const auto& curve=grade.advanced->operations[operation].curves[channel]; point=curve[std::min(i,curve.size()-1)];
    } else if(i==0) point={0,0};
    const auto offset=((operation*4+channel)*16+i)*2; points[offset]=point.x; points[offset+1]=point.y;
  }
  return points;
}
} // namespace corevideo::modules
