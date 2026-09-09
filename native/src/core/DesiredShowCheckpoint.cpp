#include "core/DesiredShowCheckpoint.h"
#include <set>

namespace corevideo::core {
namespace {
constexpr uint64_t maxSafe=9007199254740991ULL;
bool text(const std::string& s) { return !s.empty() && s.size()<=512; }
bool generation(const std::optional<uint64_t>& g) { return g && *g>0 && *g<=maxSafe; }
bool exact(const ShowSourceRef& ref) {
  return text(ref.sourceId)&&text(ref.instanceId)&&text(ref.processEpoch)&&ref.generation>0&&ref.generation<=maxSafe;
}
}
DesiredShowCheckpoint::Result DesiredShowCheckpoint::project(const Capture& c,size_t limit,size_t byteLimit) {
  Result result; result.reasons.fill(Reason::Uncaptured);
  if (!text(c.legacyAuthorityEpoch)||!c.legacyRevision||*c.legacyRevision>maxSafe||!limit||!byteLimit||
      (c.knownEmptyDomains & ~emptyOnlyDomains)) return result;
  size_t count=0,bytes=c.legacyAuthorityEpoch.size();
  const auto addBytes=[&](size_t n) { if(bytes>byteLimit||n>byteLimit-bytes) return false; bytes+=n; return true; };
  if(bytes>byteLimit) return result;
  if(c.scenes) {
    if(c.scenes->size()>limit) return result;
    count=c.scenes->size();
    for(const auto& s:*c.scenes) {
      if(s.orderedRoutes.size()>limit-count||!addBytes(s.id.size())||!addBytes(s.label.size())) return result;
      count+=s.orderedRoutes.size();
      for(const auto& r:s.orderedRoutes) {
        if(!addBytes(r.id.size())) return result;
        if(r.target.source && (!addBytes(r.target.source->sourceId.size())||!addBytes(r.target.source->instanceId.size())||!addBytes(r.target.source->processEpoch.size()))) return result;
        if(r.target.person&&!addBytes(r.target.person->id.size())) return result;
      }
    }
  }
  if(c.outputs) {
    if(c.outputs->size()>limit-count) return result;
    for(const auto& output:*c.outputs) if(!addBytes(output.id.size())) return result;
  }
  for(const auto* bus:{&c.program,&c.preview}) if(bus->sceneId&&!addBytes(bus->sceneId->size())) return result;
  result.validBasis=true; result.desired.authorityEpoch=c.legacyAuthorityEpoch; result.desired.expectedRevision=*c.legacyRevision;
  const auto support=[&](unsigned index) { result.supportedDomains|=1U<<index; result.unsupportedDomains&=~(1U<<index); result.reasons[index]=Reason::None; };
  for(unsigned i=3;i<=7;++i) if(c.knownEmptyDomains&(1U<<i)) support(i);
  if(result.supportedDomains&8) result.desired.inputs.emplace();
  if(result.supportedDomains&16) result.desired.tiles.emplace();
  if(result.supportedDomains&32) result.desired.overlays.emplace();
  if(result.supportedDomains&64) result.desired.isoSelections.emplace();
  if(result.supportedDomains&128) result.desired.audioRoutes.emplace();
  if(c.scenes) {
    std::vector<LegacySceneDto> scenes; std::set<std::string> ids; Reason reason=Reason::None;
    for(const auto& s:*c.scenes) {
      if(!text(s.id)||!ids.insert(s.id).second) {reason=Reason::Invalid;break;}
      if(!generation(s.generation)) {reason=Reason::MissingGeneration;break;}
      if(s.unsupportedPolicies) {reason=Reason::UnsupportedPolicy;break;}
      LegacySceneDto scene; scene.id=s.id; scene.label=s.label; scene.generation=*s.generation;
      std::set<std::string> routeIds;
      for(const auto& r:s.orderedRoutes) {
        if(!text(r.id)||!routeIds.insert(r.id).second||r.width<=0||r.height<=0) {reason=Reason::Invalid;break;}
        if(!generation(r.generation)) {reason=Reason::MissingGeneration;break;}
        if(r.unsupportedPolicies) {reason=Reason::UnsupportedPolicy;break;}
        if(r.target.kind==ShowRouteKind::FixedSource) {
          if(!r.target.source||r.target.person||!exact(*r.target.source)) {reason=Reason::MissingSourceIdentity;break;}
        } else if(r.target.kind!=ShowRouteKind::Blank) {reason=Reason::UnsupportedPolicy;break;}
        else if(r.target.source||r.target.person) {reason=Reason::Invalid;break;}
        scene.layers.push_back({r.id,{*r.generation,r.target,r.x,r.y,r.width,r.height,r.visible}});
      }
      if(reason!=Reason::None) break;
      scenes.push_back(std::move(scene));
    }
    result.reasons[2]=reason;
    if(reason==Reason::None) {result.desired.scenes=std::move(scenes);support(2);}
  }
  const auto bus=[&](const Bus& value,LegacyBusDto& out,unsigned index) {
    if(!value.captured) return;
    if(!value.sceneId) {
      if(value.generation) {result.reasons[index]=Reason::Invalid;return;}
      out.supplied=true;support(index);return;
    }
    if(!text(*value.sceneId)) {result.reasons[index]=Reason::Invalid;return;}
    if(!generation(value.generation)) {result.reasons[index]=Reason::MissingGeneration;return;}
    // A named bus cannot claim support when its scene definition is unsupported.
    if(!result.desired.scenes) {result.reasons[index]=result.reasons[2];return;}
    bool found=false;
    for(const auto& scene:*result.desired.scenes) if(scene.id==*value.sceneId&&scene.generation==*value.generation) found=true;
    if(!found) {result.reasons[index]=Reason::Invalid;return;}
    out={true,ShowEntityRef{*value.sceneId,*value.generation}};support(index);
  };
  bus(c.program,result.desired.program,0); bus(c.preview,result.desired.preview,1);
  if(c.outputs) {
    std::vector<LegacyNamedShowIntent<ShowOutputIntent>> outputs;std::set<std::string> ids;Reason reason=Reason::None;
    for(const auto& output:*c.outputs) {
      if(!text(output.id)||!ids.insert(output.id).second) {reason=Reason::Invalid;break;}
      if(!generation(output.generation)) {reason=Reason::MissingGeneration;break;}
      if(output.unsupportedPolicies) {reason=Reason::UnsupportedPolicy;break;}
      switch(output.kind) {
        case ShowOutputIntent::Kind::Program:case ShowOutputIntent::Kind::Stream:case ShowOutputIntent::Kind::Recorder:
        case ShowOutputIntent::Kind::VirtualCamera:case ShowOutputIntent::Kind::Monitor:break;
        default:reason=Reason::Invalid;
      }
      if(reason!=Reason::None) break;
      outputs.push_back({output.id,{*output.generation,output.kind,output.requested}});
    }
    result.reasons[8]=reason;
    if(reason==Reason::None) {result.desired.outputs=std::move(outputs);support(8);}
  }
  result.complete=result.unsupportedDomains==0;
  return result;
}
} // namespace corevideo::core
