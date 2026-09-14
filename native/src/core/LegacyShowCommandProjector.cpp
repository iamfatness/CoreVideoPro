#include "core/LegacyShowCommandProjector.h"
#include <utility>

namespace corevideo::core {
namespace {
constexpr std::uint64_t maxSafe = 9'007'199'254'740'991ULL;
bool text(const std::string& s) { return !s.empty() && s.size() <= 512; }
bool generation(std::uint64_t g) { return g > 0 && g <= maxSafe; }
bool ref(const ShowEntityRef& r) { return text(r.id) && generation(r.generation); }
bool ref(const ShowSourceRef& r) {
  return text(r.sourceId) && text(r.instanceId) && text(r.processEpoch) && generation(r.generation);
}
bool target(const ShowRouteTarget& t) {
  switch (t.kind) {
    case ShowRouteKind::FixedSource: return t.source && ref(*t.source) && !t.person;
    case ShowRouteKind::FollowPerson: return t.person && ref(*t.person) && !t.source;
    case ShowRouteKind::Blank: case ShowRouteKind::ActiveSpeaker:
    case ShowRouteKind::Spotlight: case ShowRouteKind::ScreenShare: return !t.source && !t.person;
  }
  return false;
}
template<class T> bool replaceRows(const std::vector<LegacyNamedShowIntent<T>>& rows,
    std::map<std::string, T>& output, std::vector<std::string>* order = nullptr) {
  output.clear(); if (order) order->clear();
  for (const auto& row : rows) {
    if (!text(row.id) || !output.emplace(row.id, row.intent).second) return false;
    if (order) order->push_back(row.id);
  }
  return true;
}
template<class T> bool validateEntities(const std::map<std::string,T>& before,
                                      const std::map<std::string,T>& after) {
  for (const auto& [id,value] : after) {
    if (!text(id) || !generation(value.generation)) return false;
    const auto old = before.find(id);
    if (old != before.end() && !(old->second == value) && value.generation <= old->second.generation) return false;
  }
  return true;
}
template<class T> bool orderValid(const std::vector<std::string>& order, const std::map<std::string,T>& map) {
  if (order.size() != map.size()) return false;
  std::set<std::string> seen;
  for (const auto& id : order) if (!map.contains(id) || !seen.insert(id).second) return false;
  return true;
}
bool validCandidate(const ShowStateData& before, const ShowStateData& c) {
  if (!validateEntities(before.inputs,c.inputs) || !validateEntities(before.scenes,c.scenes) ||
      !validateEntities(before.tiles,c.tiles) || !validateEntities(before.isoSelections,c.isoSelections) ||
      !validateEntities(before.audioRoutes,c.audioRoutes) || !validateEntities(before.outputs,c.outputs) ||
      !validateEntities(before.overlays,c.overlays) || !orderValid(c.inputOrder,c.inputs) || !orderValid(c.isoOrder,c.isoSelections)) return false;
  for (const auto& [id,input] : c.inputs) if (!target(input.target)) return false;
  for (const auto& [id,iso] : c.isoSelections)
    if (!target(iso.target) || (iso.target.kind != ShowRouteKind::FixedSource && iso.target.kind != ShowRouteKind::FollowPerson)) return false;
  const auto inputRef = [&](const ShowEntityRef& r) {
    const auto i = c.inputs.find(r.id);
    return ref(r) && i != c.inputs.end() && i->second.generation == r.generation;
  };
  for (const auto& [id,tiles] : c.tiles) {
    for (const auto& r : tiles.includedInputs) if (!inputRef(r)) return false;
    for (const auto& r : tiles.excludedInputs) if (!inputRef(r)) return false;
    std::set<ShowEntityRef> seen;
    for (const auto& [slot,r] : tiles.reservedSlots) if (r && (!inputRef(*r) || !seen.insert(*r).second)) return false;
  }
  for (const auto& [id,scene] : c.scenes) {
    const auto old = before.scenes.find(id);
    const std::map<std::string,ShowLayerIntent> empty;
    if (!validateEntities(old == before.scenes.end() ? empty : old->second.routes, scene.routes) || !orderValid(scene.layerOrder,scene.routes)) return false;
    for (const auto& [layerId,layer] : scene.routes) if (!target(layer.target) || layer.width <= 0 || layer.height <= 0) return false;
  }
  for (const auto& [id,overlay] : c.overlays) if (overlay.source && !ref(*overlay.source)) return false;
  for (const auto& [id,audio] : c.audioRoutes) {
    const auto out = c.outputs.find(audio.destination.id);
    if (!target(audio.source) || !ref(audio.destination) || out == c.outputs.end() || out->second.generation != audio.destination.generation) return false;
  }
  for (const auto& [id,out] : c.outputs) switch (out.kind) {
    case ShowOutputIntent::Kind::Program: case ShowOutputIntent::Kind::Stream:
    case ShowOutputIntent::Kind::Recorder: case ShowOutputIntent::Kind::VirtualCamera:
    case ShowOutputIntent::Kind::Monitor: break;
    default: return false;
  }
  for (const auto* bus : {&c.preview,&c.program}) if (*bus) {
    const auto scene = c.scenes.find((*bus)->id);
    if (!ref(**bus) || scene == c.scenes.end() || scene->second.generation != (*bus)->generation) return false;
  }
  return true;
}
std::size_t entities(const ShowStateData& c) {
  auto n = c.inputs.size()+c.scenes.size()+c.tiles.size()+c.isoSelections.size()+c.audioRoutes.size()+c.outputs.size()+c.overlays.size();
  for (const auto& [id,s] : c.scenes) n += s.routes.size();
  for (const auto& [id,t] : c.tiles) n += t.includedInputs.size()+t.excludedInputs.size()+t.reservedSlots.size();
  return n;
}
bool addBytes(std::size_t& total, std::size_t amount, std::size_t limit) {
  if (amount > limit - total) return false;
  total += amount;
  return true;
}
bool stateTextWithin(const ShowStateData& c, std::size_t limit) {
  std::size_t total = 0;
  for (const auto& [id,input] : c.inputs)
    if (!addBytes(total,id.size(),limit) || !addBytes(total,input.label.size(),limit)) return false;
  for (const auto& [id,scene] : c.scenes) {
    if (!addBytes(total,id.size(),limit) || !addBytes(total,scene.label.size(),limit)) return false;
    for (const auto& [routeId,route] : scene.routes)
      if (!addBytes(total,routeId.size(),limit)) return false;
  }
  for (const auto& [id,overlay] : c.overlays)
    if (!addBytes(total,id.size(),limit) || !addBytes(total,overlay.content.size(),limit)) return false;
  for (const auto& [id,value] : c.tiles) if (!addBytes(total,id.size(),limit)) return false;
  for (const auto& [id,value] : c.isoSelections) if (!addBytes(total,id.size(),limit)) return false;
  for (const auto& [id,value] : c.audioRoutes) if (!addBytes(total,id.size(),limit)) return false;
  for (const auto& [id,value] : c.outputs) if (!addBytes(total,id.size(),limit)) return false;
  return true;
}
bool commandTextWithin(const LegacyShowCommandDto& command, std::size_t limit) {
  std::size_t total = 0;
  const auto rows = [&](const auto& optionalRows, const auto& payloadBytes) {
    if (!optionalRows) return true;
    for (const auto& row : *optionalRows)
      if (!addBytes(total,row.id.size(),limit) || !addBytes(total,payloadBytes(row.intent),limit)) return false;
    return true;
  };
  if (!rows(command.inputs, [](const auto& value) { return value.label.size(); }) ||
      !rows(command.overlays, [](const auto& value) { return value.content.size(); }) ||
      !rows(command.isoSelections, [](const auto&) { return std::size_t{0}; }) ||
      !rows(command.audioRoutes, [](const auto&) { return std::size_t{0}; }) ||
      !rows(command.outputs, [](const auto&) { return std::size_t{0}; })) return false;
  if (command.scenes) for (const auto& scene : *command.scenes) {
    if (!addBytes(total,scene.id.size(),limit) || !addBytes(total,scene.label.size(),limit)) return false;
    for (const auto& layer : scene.layers) if (!addBytes(total,layer.id.size(),limit)) return false;
  }
  if (command.tiles) for (const auto& tiles : *command.tiles)
    if (!addBytes(total,tiles.id.size(),limit)) return false;
  return true;
}
}
LegacyShowProjection LegacyShowCommandProjector::project(const ShowStateSnapshot& base,
    const LegacyShowCommandDto& command, std::size_t limit, std::size_t textLimit) {
  using Error = LegacyShowProjection::Error;
  const auto reject = [](Error e) { return LegacyShowProjection{e,std::nullopt}; };
  if (base.authorityEpoch != command.authorityEpoch || base.revision != command.expectedRevision) return reject(Error::Conflict);
  if (!text(command.authorityEpoch) || base.revision > maxSafe || limit == 0 || limit > 1'000'000 ||
      textLimit == 0 || textLimit > 64 * 1024 * 1024) return reject(Error::Invalid);
  std::size_t incoming = 0;
  if (command.inputs) incoming += command.inputs->size();
  if (command.isoSelections) incoming += command.isoSelections->size();
  if (command.audioRoutes) incoming += command.audioRoutes->size();
  if (command.outputs) incoming += command.outputs->size();
  if (command.overlays) incoming += command.overlays->size();
  if (command.scenes) for (const auto& s : *command.scenes) incoming += 1+s.layers.size();
  if (command.tiles) for (const auto& t : *command.tiles) incoming += 1+t.includedInputs.size()+t.excludedInputs.size()+t.reservedSlots.size();
  if (incoming > limit || entities(base.data) > limit || !stateTextWithin(base.data,textLimit) ||
      !commandTextWithin(command,textLimit)) return reject(Error::Capacity);
  auto c = base.data;
  if (command.inputs && !replaceRows(*command.inputs,c.inputs,&c.inputOrder)) return reject(Error::Invalid);
  if (command.isoSelections && !replaceRows(*command.isoSelections,c.isoSelections,&c.isoOrder)) return reject(Error::Invalid);
  if (command.audioRoutes && !replaceRows(*command.audioRoutes,c.audioRoutes)) return reject(Error::Invalid);
  if (command.outputs && !replaceRows(*command.outputs,c.outputs)) return reject(Error::Invalid);
  if (command.overlays && !replaceRows(*command.overlays,c.overlays)) return reject(Error::Invalid);
  if (command.scenes) {
    c.scenes.clear();
    for (const auto& dto : *command.scenes) {
      ShowSceneIntent scene; scene.generation = dto.generation; scene.label = dto.label;
      if (!text(dto.id) || !replaceRows(dto.layers,scene.routes,&scene.layerOrder) || !c.scenes.emplace(dto.id,std::move(scene)).second) return reject(Error::Invalid);
    }
  }
  if (command.tiles) {
    c.tiles.clear();
    for (const auto& dto : *command.tiles) {
      ShowTilesIntent tiles; tiles.generation = dto.generation; tiles.autoFill = dto.autoFill; tiles.allowRosterAdditions = dto.allowRosterAdditions;
      for (const auto& r : dto.includedInputs) if (!tiles.includedInputs.insert(r).second) return reject(Error::Invalid);
      for (const auto& r : dto.excludedInputs) if (!tiles.excludedInputs.insert(r).second) return reject(Error::Invalid);
      for (const auto& slot : dto.reservedSlots) if (!tiles.reservedSlots.emplace(slot.index,slot.input).second) return reject(Error::Invalid);
      if (!text(dto.id) || !c.tiles.emplace(dto.id,std::move(tiles)).second) return reject(Error::Invalid);
    }
  }
  if (!command.preview.supplied && command.preview.scene) return reject(Error::Invalid);
  if (!command.program.supplied && command.program.scene) return reject(Error::Invalid);
  if (command.preview.supplied) c.preview = command.preview.scene;
  if (command.program.supplied) c.program = command.program.scene;
  if (entities(c) > limit || !stateTextWithin(c,textLimit)) return reject(Error::Capacity);
  if (!validCandidate(base.data,c)) return reject(Error::Invalid);
  return {Error::None,std::move(c)};
}
} // namespace corevideo::core
