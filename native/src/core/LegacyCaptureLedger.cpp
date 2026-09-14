#include "core/LegacyCaptureLedger.h"
#include <limits>
#include <random>
#include <set>
#include <stdexcept>

namespace corevideo::core {
namespace {
constexpr uint64_t maximum=9007199254740991ULL;
using Capture=DesiredShowCheckpoint::Capture;
struct Encoding {
  size_t limit;
  std::string value;
  bool valid{true};
  void text(const std::string& s) {
    if (!valid) return;
    const auto length=std::to_string(s.size());
    if(s.size()>limit || length.size()+1>limit-s.size() || value.size()>limit-s.size()-length.size()-1) {valid=false;return;}
    value+=length;value+=':';value+=s;
  }
  template<class T> void number(T n) {text(std::to_string(n));}
  void target(const ShowRouteTarget& t) {
    number(static_cast<int>(t.kind));number(bool(t.source));
    if(t.source) {text(t.source->sourceId);text(t.source->instanceId);text(t.source->processEpoch);number(t.source->generation);}
    number(bool(t.person));if(t.person) {text(t.person->id);number(t.person->generation);}
  }
};
std::string freshEpoch() {
  std::random_device random;
  std::string epoch="legacy-capture-";
  for(int i=0;i<4;++i) {epoch+=std::to_string(random());epoch+='-';}
  return epoch;
}
void routeEncoding(Encoding& e,const DesiredShowCheckpoint::Route& r) {
  e.text(r.id);e.target(r.target);e.number(r.x);e.number(r.y);e.number(r.width);e.number(r.height);
  e.number(r.visible);e.number(r.unsupportedPolicies);
}
void sceneEncoding(Encoding& e,const DesiredShowCheckpoint::Scene& s) {
  e.text(s.id);e.text(s.label);e.number(s.unsupportedPolicies);e.number(s.orderedRoutes.size());
  for(const auto& r:s.orderedRoutes) {routeEncoding(e,r);if(!e.valid)return;}
}
void outputEncoding(Encoding& e,const DesiredShowCheckpoint::Output& o) {
  e.text(o.id);e.number(static_cast<int>(o.kind));e.number(o.requested);e.number(o.unsupportedPolicies);
}
void busEncoding(Encoding& e,const DesiredShowCheckpoint::Bus& b) {
  e.number(b.captured);e.number(bool(b.sceneId));if(b.sceneId)e.text(*b.sceneId);
}
}
LegacyCaptureLedger::LegacyCaptureLedger():LegacyCaptureLedger(Config{}) {}
LegacyCaptureLedger::LegacyCaptureLedger(Config config):config_(std::move(config)) {
  if(!config_.maxEntities||config_.maxEntities>65536||!config_.maxGenerationMarks||config_.maxGenerationMarks>1'000'000||
     !config_.maxRetainedBytes||config_.maxRetainedBytes>64*1024*1024||config_.authorityEpoch.size()>512)
    throw std::invalid_argument("Invalid capture ledger bounds");
  if(config_.authorityEpoch.empty())config_.authorityEpoch=freshEpoch();
  auto snapshot=std::make_shared<Snapshot>();snapshot->authorityEpoch=config_.authorityEpoch;
  auto state=std::make_shared<State>();state->snapshot=std::move(snapshot);state_=std::move(state);
}
std::shared_ptr<const LegacyCaptureLedger::Snapshot> LegacyCaptureLedger::snapshot() const {
  std::lock_guard lock(mutex_);return state_->snapshot;
}
LegacyCaptureLedger::Result LegacyCaptureLedger::publish(Capture capture, std::vector<Retirement> retirements) {
  std::shared_ptr<const State> base;
  {std::lock_guard lock(mutex_);base=state_;}
  const auto reject=[&](Status s){return Result{s,base->snapshot};};
  if(retirements.size()>config_.maxEntities)return reject(Status::Capacity);
  size_t entities=0;
  if(capture.scenes) {
    if(capture.scenes->size()>config_.maxEntities)return reject(Status::Capacity);
    entities=capture.scenes->size();
    for(const auto& scene:*capture.scenes) {
      if(scene.orderedRoutes.size()>config_.maxEntities-entities)return reject(Status::Capacity);
      entities+=scene.orderedRoutes.size();
    }
  }
  if(capture.outputs) {if(capture.outputs->size()>config_.maxEntities-entities)return reject(Status::Capacity);entities+=capture.outputs->size();}
  Encoding whole{config_.maxRetainedBytes};
  busEncoding(whole,capture.program);busEncoding(whole,capture.preview);whole.number(capture.knownEmptyDomains);
  whole.number(bool(capture.scenes));if(capture.scenes)whole.number(capture.scenes->size());
  if(capture.scenes)for(const auto& s:*capture.scenes){sceneEncoding(whole,s);if(!whole.valid)return reject(Status::Capacity);}
  whole.number(bool(capture.outputs));if(capture.outputs)whole.number(capture.outputs->size());
  if(capture.outputs)for(const auto& o:*capture.outputs){outputEncoding(whole,o);if(!whole.valid)return reject(Status::Capacity);}
  if(!whole.valid)return reject(Status::Capacity);
  auto next=std::make_shared<State>();next->marks=base->marks;next->canonical=std::move(whole.value);
  std::set<std::vector<std::string>> seen;
  Status failure=Status::Changed;
  const auto allocate=[&](std::vector<std::string> key,std::string value)->std::optional<uint64_t> {
    for(const auto& part:key)if(part.empty()||part.size()>512){failure=Status::Invalid;return {};}
    if(!seen.insert(key).second){failure=Status::Invalid;return {};}
    auto found=next->marks.find(key);
    if(found==next->marks.end()) {
      if(next->marks.size()>=config_.maxGenerationMarks){failure=Status::Capacity;return {};}
      found=next->marks.emplace(std::move(key),Mark{}).first;
    }
    auto& mark=found->second;
    if(!mark.activeValue||*mark.activeValue!=value) {
      if(mark.generation==maximum){failure=Status::Exhausted;return {};}
      ++mark.generation;
    }
    mark.activeValue=std::move(value);return mark.generation;
  };
  if(capture.scenes)for(auto& s:*capture.scenes) {
    Encoding value{config_.maxRetainedBytes};sceneEncoding(value,s);
    s.generation=allocate({"scene",s.id},std::move(value.value));if(!s.generation)return reject(failure);
    for(auto& r:s.orderedRoutes) {
      Encoding route{config_.maxRetainedBytes};routeEncoding(route,r);
      r.generation=allocate({"route",s.id,r.id},std::move(route.value));if(!r.generation)return reject(failure);
    }
  }
  if(capture.outputs)for(auto& o:*capture.outputs) {
    Encoding value{config_.maxRetainedBytes};outputEncoding(value,o);
    o.generation=allocate({"output",o.id},std::move(value.value));if(!o.generation)return reject(failure);
  }
  // A bus uncues a scene without deleting it. Retire only explicit identity
  // deletions supplied by the same accepted command boundary.
  for(const auto& retired:retirements) {
    std::vector<std::string> key;
    switch(retired.kind) {
      case Retirement::Kind::Scene:key={"scene",retired.id};break;
      case Retirement::Kind::Route:key={"route",retired.sceneId,retired.id};break;
      case Retirement::Kind::Output:key={"output",retired.id};break;
      default:return reject(Status::Invalid);
    }
    for(const auto& part:key)if(part.empty()||part.size()>512)return reject(Status::Invalid);
    if(seen.contains(key))return reject(Status::Invalid);
    const auto found=next->marks.find(key);
    if(found==next->marks.end())return reject(Status::Invalid);
    found->second.activeValue.reset();
    if(retired.kind==Retirement::Kind::Scene)for(auto& [routeKey,mark]:next->marks)
      if(routeKey.size()==3&&routeKey[0]=="route"&&routeKey[1]==retired.id)mark.activeValue.reset();
  }
  if(base->snapshot->hasCapture&&next->canonical==base->canonical&&next->marks==base->marks)
    return reject(Status::Unchanged);
  for(auto* bus:{&capture.program,&capture.preview}) {
    bus->generation.reset();
    if(!bus->captured||!bus->sceneId||!capture.scenes)continue;
    for(const auto& scene:*capture.scenes)if(scene.id==*bus->sceneId)bus->generation=scene.generation;
  }
  if(base->snapshot->revision==maximum)return reject(Status::Exhausted);
  capture.legacyAuthorityEpoch=config_.authorityEpoch;capture.legacyRevision=base->snapshot->revision+1;
  auto projected=DesiredShowCheckpoint::project(capture,config_.maxEntities,config_.maxRetainedBytes);
  if(!projected.validBasis)return reject(Status::Invalid);
  // Conservatively account retained capture+projection+canonical values and mark
  // payloads, including tombstone key storage; not just the active entity count.
  size_t retained=sizeof(State)+sizeof(Snapshot);
  const auto budget=[&](size_t n){if(n>config_.maxRetainedBytes-retained)return false;retained+=n;return true;};
  if(retained>config_.maxRetainedBytes||!budget(config_.authorityEpoch.size()*4))return reject(Status::Capacity);
  if(entities>config_.maxRetainedBytes/1024||!budget(entities*1024))return reject(Status::Capacity);
  if(next->canonical.size()>config_.maxRetainedBytes/4||!budget(next->canonical.size()*4))return reject(Status::Capacity);
  for(const auto& [key,mark]:next->marks) {
    if(!budget(sizeof(Mark)+sizeof(std::vector<std::string>)+128))return reject(Status::Capacity);
    for(const auto& part:key)if(!budget(sizeof(std::string)+part.size()+1))return reject(Status::Capacity);
    if(mark.activeValue&&!budget(mark.activeValue->size()+1))return reject(Status::Capacity);
  }
  auto snapshot=std::make_shared<Snapshot>();snapshot->authorityEpoch=config_.authorityEpoch;
  snapshot->revision=*capture.legacyRevision;snapshot->capture=capture;snapshot->projection=std::move(projected);snapshot->hasCapture=true;
  next->snapshot=snapshot;
  {std::lock_guard lock(mutex_);if(state_!=base)return {Status::Conflict,state_->snapshot};state_=std::move(next);}
  return {Status::Changed,std::move(snapshot)};
}
} // namespace corevideo::core
