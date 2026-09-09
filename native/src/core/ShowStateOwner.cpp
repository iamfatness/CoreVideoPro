#include "core/ShowStateOwner.h"
#include <stdexcept>
#include <utility>

namespace corevideo::core {
ShowStateOwner::ShowStateOwner(const ShowStateOwner& other) {
  std::lock_guard lock(other.mutex_);
  current_ = other.current_; generationMarks_ = other.generationMarks_;
  maxGenerationMarks_ = other.maxGenerationMarks_;
}
namespace {
constexpr std::uint64_t maxSafe = 9'007'199'254'740'991ULL;
bool validRef(const ShowEntityRef& ref) {
  return !ref.id.empty() && ref.generation > 0 && ref.generation <= maxSafe;
}
bool validRef(const ShowSourceRef& ref) {
  return !ref.sourceId.empty() && !ref.instanceId.empty() && !ref.processEpoch.empty() &&
      ref.generation > 0 && ref.generation <= maxSafe;
}
bool validTarget(const ShowRouteTarget& target) {
  switch (target.kind) {
    case ShowRouteKind::FixedSource: return target.source && validRef(*target.source) && !target.person;
    case ShowRouteKind::FollowPerson: return target.person && validRef(*target.person) && !target.source;
    case ShowRouteKind::Blank:
    case ShowRouteKind::ActiveSpeaker:
    case ShowRouteKind::Spotlight:
    case ShowRouteKind::ScreenShare: return !target.source && !target.person;
  }
  return false;
}
template<class Map> bool validEntities(const Map& entries) {
  for (const auto& [id, value] : entries)
    if (id.empty() || value.generation == 0 || value.generation > maxSafe) return false;
  return true;
}
bool validData(const ShowStateData& data) {
  if (!validEntities(data.inputs) || !validEntities(data.scenes) ||
      !validEntities(data.overlays) || !validEntities(data.audioRoutes) ||
      !validEntities(data.outputs) || !validEntities(data.tiles) ||
      !validEntities(data.isoSelections)) return false;
  for (const auto& [id, input] : data.inputs) if (!validTarget(input.target)) return false;
  std::set<std::string> inputOrder;
  if (data.inputOrder.size() != data.inputs.size()) return false;
  for (const auto& id : data.inputOrder)
    if (!data.inputs.contains(id) || !inputOrder.insert(id).second) return false;
  std::set<std::string> isoOrder;
  if (data.isoOrder.size() != data.isoSelections.size()) return false;
  for (const auto& id : data.isoOrder)
    if (!data.isoSelections.contains(id) || !isoOrder.insert(id).second) return false;
  for (const auto& [id, iso] : data.isoSelections)
    if ((iso.target.kind != ShowRouteKind::FixedSource &&
         iso.target.kind != ShowRouteKind::FollowPerson) || !validTarget(iso.target)) return false;
  const auto validInput = [&](const ShowEntityRef& ref) {
    const auto found = data.inputs.find(ref.id);
    return validRef(ref) && found != data.inputs.end() && found->second.generation == ref.generation;
  };
  for (const auto& [id, tiles] : data.tiles) {
    for (const auto& ref : tiles.includedInputs) if (!validInput(ref)) return false;
    for (const auto& ref : tiles.excludedInputs) if (!validInput(ref)) return false;
    std::set<ShowEntityRef> reserved;
    for (const auto& [slot, ref] : tiles.reservedSlots)
      if (ref && (!validInput(*ref) || !reserved.insert(*ref).second)) return false;
  }
  for (const auto& [id, scene] : data.scenes) {
    if (!validEntities(scene.routes) || scene.layerOrder.size() != scene.routes.size()) return false;
    std::set<std::string> ordered;
    for (const auto& routeId : scene.layerOrder)
      if (!scene.routes.contains(routeId) || !ordered.insert(routeId).second) return false;
    for (const auto& [routeId, route] : scene.routes)
      if (!validTarget(route.target) || route.width <= 0 || route.height <= 0) return false;
  }
  for (const auto& [id, overlay] : data.overlays)
    if (overlay.source && !validRef(*overlay.source)) return false;
  for (const auto& [id, audio] : data.audioRoutes) {
    if (!validTarget(audio.source) || !validRef(audio.destination)) return false;
    const auto output = data.outputs.find(audio.destination.id);
    if (output == data.outputs.end() || output->second.generation != audio.destination.generation) return false;
  }
  for (const auto& [id, output] : data.outputs) {
    switch (output.kind) {
      case ShowOutputIntent::Kind::Program: case ShowOutputIntent::Kind::Stream:
      case ShowOutputIntent::Kind::Recorder: case ShowOutputIntent::Kind::VirtualCamera:
      case ShowOutputIntent::Kind::Monitor: break;
      default: return false;
    }
  }
  for (const auto* bus : {&data.preview, &data.program}) {
    if (!*bus) continue;
    if (!validRef(**bus)) return false;
    const auto scene = data.scenes.find((*bus)->id);
    if (scene == data.scenes.end() || scene->second.generation != (*bus)->generation) return false;
  }
  return true;
}
}  // namespace

ShowRouteResolution classifyShowRoute(const ShowRouteTarget& target,
                                     const std::set<ShowSourceRef>& available) {
  if (!validTarget(target)) return ShowRouteResolution::Missing;
  if (target.kind == ShowRouteKind::Blank) return ShowRouteResolution::Blank;
  if (target.kind == ShowRouteKind::FixedSource)
    return available.contains(*target.source) ? ShowRouteResolution::Available : ShowRouteResolution::Missing;
  return ShowRouteResolution::RequiresSelection;
}
ShowStateOwner::ShowStateOwner(std::string authorityEpoch, std::size_t maxGenerationMarks)
    : maxGenerationMarks_(maxGenerationMarks) {
  if (authorityEpoch.empty()) throw std::invalid_argument("ShowState authority epoch must be nonempty");
  if (maxGenerationMarks_ == 0) throw std::invalid_argument("ShowState generation capacity must be positive");
  generationMarks_ = std::make_shared<const GenerationMarks>();
  current_ = std::make_shared<const ShowStateSnapshot>(ShowStateSnapshot{std::move(authorityEpoch), 0, {}});
}
std::shared_ptr<const ShowStateSnapshot> ShowStateOwner::snapshot() const {
  std::lock_guard lock(mutex_);
  return current_;
}
ShowStateUpdateResult ShowStateOwner::replace(const std::string& expectedAuthorityEpoch,
                                            std::uint64_t expectedRevision, ShowStateData candidate) {
  // Capture a consistent immutable base, then do all traversal/allocation outside
  // the publication lock. A competing writer causes Conflict at the second check.
  std::shared_ptr<const ShowStateSnapshot> base;
  std::shared_ptr<const GenerationMarks> oldMarks;
  {
    std::lock_guard lock(mutex_);
    base = current_;
    oldMarks = generationMarks_;
  }
  if (base->authorityEpoch != expectedAuthorityEpoch || base->revision != expectedRevision)
    return {ShowStateUpdateStatus::Conflict, base};
  if (!validData(candidate)) return {ShowStateUpdateStatus::Invalid, base};
  const bool unchanged = base->data == candidate;
  if (unchanged) {
    std::lock_guard lock(mutex_);
    return current_ == base
        ? ShowStateUpdateResult{ShowStateUpdateStatus::Unchanged, current_}
        : ShowStateUpdateResult{ShowStateUpdateStatus::Conflict, current_};
  }
  auto marks = std::make_shared<GenerationMarks>(*oldMarks);
  bool capacityExceeded = false;
  const auto checkEntities = [&](const auto& before, const auto& after, std::vector<std::string> prefix) {
    for (const auto& [id, value] : after) {
      auto key = prefix; key.push_back(id);
      const auto previous = before.find(id);
      const auto mark = marks->find(key);
      const bool same = previous != before.end() && previous->second == value;
      if (!same && mark != marks->end() && value.generation <= mark->second) return false;
      if (mark == marks->end() && marks->size() >= maxGenerationMarks_) {
        capacityExceeded = true;
        return false;
      }
      (*marks)[std::move(key)] = value.generation;
    }
    return true;
  };
  const auto& before = base->data;
  if (!checkEntities(before.inputs, candidate.inputs, {"input"}) ||
      !checkEntities(before.scenes, candidate.scenes, {"scene"}) ||
      !checkEntities(before.overlays, candidate.overlays, {"overlay"}) ||
      !checkEntities(before.audioRoutes, candidate.audioRoutes, {"audioRoute"}) ||
      !checkEntities(before.outputs, candidate.outputs, {"output"}) ||
      !checkEntities(before.tiles, candidate.tiles, {"tiles"}) ||
      !checkEntities(before.isoSelections, candidate.isoSelections, {"isoSelection"}))
    return {capacityExceeded ? ShowStateUpdateStatus::CapacityExceeded
                             : ShowStateUpdateStatus::Invalid, base};
  for (const auto& [id, scene] : candidate.scenes) {
    const auto previous = before.scenes.find(id);
    const std::map<std::string, ShowLayerIntent> empty;
    if (!checkEntities(previous == before.scenes.end() ? empty : previous->second.routes,
                       scene.routes, {"route", id}))
      return {capacityExceeded ? ShowStateUpdateStatus::CapacityExceeded
                               : ShowStateUpdateStatus::Invalid, base};
  }
  if (!unchanged && base->revision == maxSafe) return {ShowStateUpdateStatus::RevisionExhausted, base};
  auto next = std::make_shared<const ShowStateSnapshot>(ShowStateSnapshot{
      expectedAuthorityEpoch, base->revision + 1, std::move(candidate)});
  ShowStateUpdateResult result;
  {
    std::lock_guard lock(mutex_);
    if (current_ != base) return {ShowStateUpdateStatus::Conflict, current_};
    current_ = std::move(next);
    generationMarks_ = std::move(marks);
    result = {ShowStateUpdateStatus::Changed, current_};
  }
  // base/oldMarks retain old state until after the publication lock is released.
  return result;
}
}  // namespace corevideo::core
