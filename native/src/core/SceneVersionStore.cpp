#include "core/SceneVersionStore.h"
#include <stdexcept>

namespace corevideo::core {
namespace {
constexpr uint64_t maxSafe=9007199254740991ULL;
bool text(const std::string& s){return !s.empty()&&s.size()<=512;}
bool generation(uint64_t g){return g>0&&g<=maxSafe;}
bool validRef(const SceneVersionRef& r){return text(r.authorityEpoch)&&text(r.sceneId)&&generation(r.sceneGeneration)&&generation(r.version);}
std::optional<size_t> payloadSize(const SceneVersionRef& ref,const ShowSceneIntent& scene) {
  if(!validRef(ref)||!generation(scene.generation)||scene.label.size()>4096||scene.routes.size()>4096||scene.layerOrder.size()!=scene.routes.size())return {};
  size_t bytes=ref.authorityEpoch.size()+ref.sceneId.size()+scene.label.size()+sizeof(ImmutableSceneVersion);
  std::set<std::string> ordered;
  for(const auto& id:scene.layerOrder)if(!scene.routes.contains(id)||!ordered.insert(id).second)return {};
  for(const auto& [id,r]:scene.routes) {
    if(!text(id)||!generation(r.generation)||r.width<=0||r.height<=0)return {};
    bytes+=id.size()*2+sizeof(ShowLayerIntent)+128;
    const auto& t=r.target;
    switch(t.kind) {
      case ShowRouteKind::FixedSource:
        if(!t.source||t.person||!text(t.source->sourceId)||!text(t.source->instanceId)||!text(t.source->processEpoch)||!generation(t.source->generation))return {};
        bytes+=t.source->sourceId.size()+t.source->instanceId.size()+t.source->processEpoch.size();break;
      case ShowRouteKind::FollowPerson:
        if(!t.person||t.source||!text(t.person->id)||!generation(t.person->generation))return {};
        bytes+=t.person->id.size();break;
      case ShowRouteKind::Blank:case ShowRouteKind::ActiveSpeaker:case ShowRouteKind::Spotlight:case ShowRouteKind::ScreenShare:
        if(t.person||t.source)return {};break;
      default:return {};
    }
  }
  return bytes;
}
}
SceneVersionStore::SceneVersionStore(std::string epoch):SceneVersionStore(std::move(epoch),Limits{}){}
SceneVersionStore::SceneVersionStore(std::string epoch,Limits limits):epoch_(std::move(epoch)),limits_(limits) {
  if(!text(epoch_)||!limits.versions||limits.versions>65536||!limits.scenes||limits.scenes>65536||
      !limits.retiredEpochs||limits.retiredEpochs>65536||!limits.payloadBytes||limits.payloadBytes>64*1024*1024)
    throw std::invalid_argument("Invalid scene version store bounds");
}
SceneVersionStore::Result SceneVersionStore::publish(SceneVersionRef ref,ShowSceneIntent scene,std::optional<SceneVersionRef> expected) {
  const auto size=payloadSize(ref,scene);
  if(!size||(expected&&!validRef(*expected)))return {Status::Invalid,{}};
  auto frozen=std::make_shared<const ImmutableSceneVersion>(ImmutableSceneVersion{ref,scene});
  std::lock_guard lock(mutex_);
  if(ref.authorityEpoch!=epoch_)return {Status::Stale,{}};
  const auto existingHead=scenes_.find(ref.sceneId);
  if(existingHead!=scenes_.end()&&(!existingHead->second.active||existingHead->second.generation!=ref.sceneGeneration)) {
    if(expected||existingHead->second.active||ref.sceneGeneration<=existingHead->second.generation)return {Status::Stale,{}};
  }
  const auto prior=versions_.find(ref);
  if(prior!=versions_.end())return prior->second.lease->scene==frozen->scene ? Result{Status::Unchanged,prior->second.lease}:Result{Status::Conflict,{}};
  if(existingHead!=scenes_.end()&&existingHead->second.active) {
    if(!expected||*expected!=existingHead->second.ref)return {Status::Conflict,{}};
    if(ref.sceneGeneration!=expected->sceneGeneration||ref.version<=existingHead->second.highVersion)return {Status::Stale,{}};
  } else if(expected)return {Status::Stale,{}};
  if(versions_.size()>=limits_.versions||*size>limits_.payloadBytes-bytes_||
      (existingHead==scenes_.end()&&scenes_.size()>=limits_.scenes))return {Status::Capacity,{}};
  // Stage both allocations before publishing either container mutation.
  std::map<SceneVersionRef,Entry> stagedVersions;
  stagedVersions.emplace(ref,Entry{frozen,*size});
  std::map<std::string,SceneHead> stagedHeads;
  stagedHeads.emplace(ref.sceneId,SceneHead{ref.sceneGeneration,ref.version,true,ref});
  versions_.insert(stagedVersions.extract(stagedVersions.begin()));
  if(existingHead!=scenes_.end())scenes_.erase(existingHead);
  scenes_.insert(stagedHeads.extract(stagedHeads.begin()));
  bytes_+=*size;
  return {Status::Applied,std::move(frozen)};
}
SceneVersionStore::Result SceneVersionStore::resolve(const SceneVersionRef& ref) const {
  std::lock_guard lock(mutex_);
  if(!validRef(ref))return {Status::Invalid,{}};
  const auto scene=scenes_.find(ref.sceneId);
  if(ref.authorityEpoch!=epoch_||scene==scenes_.end()||!scene->second.active||scene->second.generation!=ref.sceneGeneration)return {Status::Stale,{}};
  const auto found=versions_.find(ref);return found==versions_.end()?Result{Status::NotFound,{}}:Result{Status::Unchanged,found->second.lease};
}
SceneVersionStore::Result SceneVersionStore::head(const std::string& id) const {
  std::lock_guard lock(mutex_);
  const auto scene=scenes_.find(id);if(scene==scenes_.end()||!scene->second.active)return {Status::NotFound,{}};
  return {Status::Unchanged,versions_.at(scene->second.ref).lease};
}
SceneVersionStore::Status SceneVersionStore::erase(const SceneVersionRef& expected) {
  std::lock_guard lock(mutex_);const auto found=scenes_.find(expected.sceneId);
  if(!validRef(expected))return Status::Invalid;
  if(expected.authorityEpoch!=epoch_||found==scenes_.end()||!found->second.active||found->second.ref!=expected)return Status::Stale;
  found->second.active=false;return Status::Applied;
}
SceneVersionStore::Status SceneVersionStore::evict(const SceneVersionRef& ref) {
  Lease retired;
  std::lock_guard lock(mutex_);const auto found=versions_.find(ref);
  if(found==versions_.end())return Status::NotFound;
  const auto scene=scenes_.find(ref.sceneId);
  if((scene!=scenes_.end()&&scene->second.active&&scene->second.ref==ref)||found->second.lease.use_count()>1)return Status::Pinned;
  bytes_-=found->second.bytes;retired=std::move(found->second.lease);versions_.erase(found);return Status::Applied;
}
SceneVersionStore::Status SceneVersionStore::restart(const std::string& expected,std::string next) {
  std::map<SceneVersionRef,Entry> retired;
  std::map<std::string,SceneHead> oldScenes;
  std::lock_guard lock(mutex_);
  if(!text(next))return Status::Invalid;
  if(expected!=epoch_||retiredEpochs_.contains(next))return Status::Stale;
  if(next==epoch_)return Status::Unchanged;
  if(retiredEpochs_.size()>=limits_.retiredEpochs)return Status::Capacity;
  retiredEpochs_.insert(epoch_);epoch_=std::move(next);retired.swap(versions_);oldScenes.swap(scenes_);bytes_=0;
  return Status::Applied;
}
}
