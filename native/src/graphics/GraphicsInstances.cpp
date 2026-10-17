#include "graphics/GraphicsInstances.h"
#include "contracts/Lifecycle.h"
#include <algorithm>
#include <cmath>
#include <limits>
#include <random>
#include <sstream>
#include <set>
#include <chrono>

namespace corevideo::graphics {
using rpc::Json;

namespace {
Json styleState(const compositor::LowerThirdAppearance& a) {
  return Json::Object{{"version", 1}, {"preset", a.preset}, {"anchor", a.anchor}, {"fontFamily", a.fontFamily},
      {"nameColor", a.nameColor}, {"titleColor", a.titleColor}, {"backgroundColor", a.backgroundColor},
      {"accentColor", a.accentColor}, {"nameSize", a.nameSize}, {"titleSize", a.titleSize},
      {"backgroundOpacity", a.backgroundOpacity}, {"padding", a.padding}, {"width", a.width},
      {"cornerRadius", a.cornerRadius}, {"safeOffsetX", a.safeOffsetX}, {"safeOffsetY", a.safeOffsetY},
      {"logoScale", a.logoScale}, {"showLogo", a.showLogo}};
}
}


GraphicsInstances::GraphicsInstances(std::string epoch, std::function<double()> monotonicMs)
    : monotonicMs_(monotonicMs ? std::move(monotonicMs) : [] {
        return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now().time_since_epoch()).count();
      }), epoch_(std::move(epoch)) {
  if (epoch_.empty()) {
    std::random_device random;
    std::ostringstream value;
    value << "graphics-" << std::hex << random() << '-' << random() << '-' << random() << '-' << random();
    epoch_ = value.str();
  }
  publish();
}

bool GraphicsInstances::validId(const std::string& value) {
  if (value.empty() || value.size() > 64 || value[0] < 'a' || value[0] > 'z') return false;
  return std::all_of(value.begin(), value.end(), [](char c) {
    return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-';
  });
}

bool GraphicsInstances::readRevision(const Json& request, Revision& revision) {
  if (const auto* animations = request.get("animations")) {
    if (!animations->isObject()) return false;
    for (const auto& [key, value] : animations->asObject()) {
      if (key == "in") { if (!GraphicsTransition::read(value, revision.entrance)) return false; }
      else if (key == "out") { if (!GraphicsTransition::read(value, revision.exit)) return false; }
      else return false;
    }
  }
  if (const auto* fields = request.get("fields")) {
    if (!fields->isObject()) return false;
    for (const auto& [key, value] : fields->asObject()) {
      if (!value.isString()) return false;
      if (key == "name" && value.asString().size() <= 256) revision.name = value.asString();
      else if (key == "title" && value.asString().size() <= 512) revision.title = value.asString();
      else return false;
    }
  }
  if (const auto* style = request.get("style")) {
    if (!style->isObject()) return false;
    auto& a = revision.appearance;
    const std::map<std::string, std::string*> strings{
        {"preset", &a.preset}, {"anchor", &a.anchor}, {"fontFamily", &a.fontFamily},
        {"nameColor", &a.nameColor}, {"titleColor", &a.titleColor},
        {"backgroundColor", &a.backgroundColor}, {"accentColor", &a.accentColor}};
    const std::map<std::string, double*> numbers{
        {"nameSize", &a.nameSize}, {"titleSize", &a.titleSize}, {"backgroundOpacity", &a.backgroundOpacity},
        {"padding", &a.padding}, {"width", &a.width}, {"cornerRadius", &a.cornerRadius},
        {"safeOffsetX", &a.safeOffsetX}, {"safeOffsetY", &a.safeOffsetY}, {"logoScale", &a.logoScale}};
    for (const auto& [key, value] : style->asObject()) {
      if (const auto found = strings.find(key); found != strings.end()) {
        if (!value.isString()) return false;
        *found->second = value.asString();
      } else if (const auto found = numbers.find(key); found != numbers.end()) {
        if (!value.isNumber()) return false;
        *found->second = value.asNumber();
      } else if (key == "showLogo") {
        if (!value.isBool()) return false;
        a.showLogo = value.asBool();
      } else if (key != "version" || !value.isNumber() || value.asNumber() != 1) return false;
    }
    if (!a.valid()) return false;
  }
  if (const auto* source = request.get("sourceId")) {
    if (!source->isString() || source->asString().size() > 256) return false;
    revision.sourceId = source->asString();
  }
  return true;
}

bool GraphicsInstances::finish(const std::string& id, bool accepted, const std::string& reason) {
  lastResult_ = Json::Object{{"requestId", id}, {"accepted", accepted}, {"reason", reason}};
  if (!pendingFingerprint_.empty()) {
    operations_.push_back({pendingClient_, id, std::move(pendingFingerprint_), lastResult_});
    if (operations_.size() > 32) operations_.pop_front();
  }
  publish();
  return accepted;
}

bool GraphicsInstances::apply(const Json& command) {
  advanceAnimations();
  pendingFingerprint_.clear(); pendingClient_.clear();
  const auto type = command.getString("type");
  const auto* body = command.get("graphics");
  if (!body || !body->isObject()) return finish("", false, "missing-graphics-request");
  const auto requestId = body->getString("requestId");
  if (!contracts::validateGraphicsOperationIdentity(*body) || !validId(requestId) || body->getNumber("schemaVersion", 0) != 1)
    return finish(requestId.substr(0, 64), false, "invalid-graphics-request");
  if (body->getString("coreEpoch") != epoch_) return finish(requestId, false, "core-epoch-conflict");
  pendingClient_ = body->getString("clientId");
  if (!validId(pendingClient_)) return finish(requestId, false, "invalid-client-id");
  // Bound before serializing a dedup fingerprint, and refuse unsupported knobs.
  static const std::set<std::string> common{"schemaVersion", "requestId", "clientId", "coreEpoch", "instanceId", "expectedRevision"};
  static const std::map<std::string, std::set<std::string>> actionKeys{
      {"graphics-instance-create", {"templateId", "templateVersion", "scope", "sceneId", "sourceId", "fields", "style", "animations"}},
      {"graphics-instance-update", {"sourceId", "fields", "style", "target", "animations"}},
      {"graphics-instance-cue", {}}, {"graphics-instance-trigger", {"action"}},
      {"graphics-instance-remove", {}}, {"graphics-program-clear", {"channel"}}};
  const auto allowed = actionKeys.find(type);
  if (allowed == actionKeys.end()) return finish(requestId, false, "unsupported-graphics-command");
  for (const auto& [key, value] : body->asObject()) {
    if (!common.count(key) && !allowed->second.count(key)) return finish(requestId, false, "unsupported-request-field");
    if (key == "animations") {
      if (!value.isObject() || value.asObject().size() > 2) return finish(requestId, false, "invalid-animations");
      for (const auto& [phase, transition] : value.asObject()) {
        GraphicsTransition parsed;
        if ((phase != "in" && phase != "out") || !GraphicsTransition::read(transition, parsed))
          return finish(requestId, false, "invalid-animations");
      }
    } else if (key == "fields" || key == "style") {
      if (!value.isObject() || value.asObject().size() > 24) return finish(requestId, false, "invalid-fields-or-style");
      for (const auto& [field, item] : value.asObject())
        if (field.size() > 64 || (item.isString() ? item.asString().size() > 512 :
            !(item.isBool() || (item.isNumber() && std::isfinite(item.asNumber())))))
          return finish(requestId, false, "invalid-fields-or-style");
    } else if (key == "schemaVersion" || key == "expectedRevision" || key == "templateVersion") {
      if (!value.isNumber() || !std::isfinite(value.asNumber()) || std::floor(value.asNumber()) != value.asNumber() ||
          value.asNumber() < 0 || value.asNumber() > 9007199254740991.)
        return finish(requestId, false, "invalid-revision");
    } else if (!value.isString() || value.asString().size() > 256) return finish(requestId, false, "invalid-request-value");
  }
  const auto fingerprint = type + ":" + body->stringify();
  for (const auto& previous : operations_) if (previous.clientId == pendingClient_ && previous.requestId == requestId) {
    if (previous.fingerprint != fingerprint) return finish(requestId, false, "request-id-reused");
    lastResult_ = previous.result; publish();
    return lastResult_.get("accepted")->asBool();
  }
  pendingFingerprint_ = fingerprint;
  if (type == "graphics-program-clear") {
    // This compatibility slice supports an explicit all-graphics clear only.
    // Do not silently interpret a selective channel request as clearing all.
    if (body->getString("channel") != "all") return finish(requestId, false, "unsupported-channel");
    for (auto& [_, instance] : instances_) {
      instance.program.reset(); instance.showPending = instance.updatePending = false;
      instance.firstDrawSubmittedFrame = 0;
      instance.timeline.clear(); instance.visibility = "hidden";
    }
    return finish(requestId, true, "cleared");
  }
  const auto id = body->getString("instanceId");
  if (!validId(id)) return finish(requestId, false, "invalid-instance-id");
  auto found = instances_.find(id);
  if (type == "graphics-instance-create") {
    if (found != instances_.end()) return finish(requestId, false, "instance-already-exists");
    if (instances_.size() >= 8) return finish(requestId, false, "instance-capacity");
    const auto templateId = body->getString("templateId");
    if (templateId != "compact-solid" && templateId != "minimal-accent" && templateId != "broadcast")
      return finish(requestId, false, "template-not-installed");
    if (body->getNumber("templateVersion", 0) != 1) return finish(requestId, false, "template-version-not-installed");
    Instance instance;
    instance.id = id; instance.templateId = templateId;
    instance.scope = body->getString("scope"); instance.sceneId = body->getString("sceneId");
    if ((instance.scope != "global" && instance.scope != "scene") ||
        (instance.scope == "scene" && (instance.sceneId.empty() || instance.sceneId.size() > 256)) ||
        (instance.scope == "global" && !instance.sceneId.empty()))
      return finish(requestId, false, "invalid-scope");
    Revision revision;
    revision.number = ++revisionClock_;
    revision.appearance.enabled = true; revision.appearance.preset = templateId;
    if (templateId == "minimal-accent") {
      revision.appearance.backgroundOpacity = 0; revision.appearance.nameSize = 40;
      revision.appearance.titleSize = 26; revision.appearance.cornerRadius = 0;
    } else if (templateId == "broadcast") {
      revision.appearance.nameSize = 52; revision.appearance.titleSize = 32;
      revision.appearance.padding = 24; revision.appearance.width = .75;
      revision.appearance.cornerRadius = 0; revision.appearance.backgroundOpacity = 1;
    }
    if (!readRevision(*body, revision)) return finish(requestId, false, "invalid-fields-or-style");
    instance.draft = std::make_shared<const Revision>(std::move(revision));
    instances_.emplace(id, std::move(instance));
    return finish(requestId, true, "created-hidden");
  }
  if (found == instances_.end()) return finish(requestId, false, "instance-not-found");
  auto& instance = found->second;
  const double expected = body->getNumber("expectedRevision", -1);
  if (expected != static_cast<double>(instance.draft->number)) return finish(requestId, false, "revision-conflict");
  if (type == "graphics-instance-update") {
    const auto target = body->getString("target");
    if (target != "cue" && target != "program") return finish(requestId, false, "invalid-target");
    Revision revision = *instance.draft;
    if (!readRevision(*body, revision)) return finish(requestId, false, "invalid-fields-or-style");
    if (target == "program" && revision.name.empty()) return finish(requestId, false, "required-field-empty");
    // Preserve explicit blanks. Same values must not allocate/raster or replay.
    const auto& old = *instance.draft;
    const auto& a = revision.appearance; const auto& b = old.appearance;
    const bool sameStyle = a.preset == b.preset && a.anchor == b.anchor && a.fontFamily == b.fontFamily &&
        a.nameColor == b.nameColor && a.titleColor == b.titleColor && a.backgroundColor == b.backgroundColor &&
        a.accentColor == b.accentColor && a.nameSize == b.nameSize && a.titleSize == b.titleSize &&
        a.backgroundOpacity == b.backgroundOpacity && a.padding == b.padding && a.width == b.width &&
        a.cornerRadius == b.cornerRadius && a.safeOffsetX == b.safeOffsetX && a.safeOffsetY == b.safeOffsetY &&
        a.logoScale == b.logoScale && a.showLogo == b.showLogo;
    if (revision.name == old.name && revision.title == old.title && revision.sourceId == old.sourceId && sameStyle &&
        revision.entrance == old.entrance && revision.exit == old.exit) {
      if (target == "program" && instance.program && instance.program != instance.draft) {
        instance.updatePending = true;
        if (instance.readyRevision == instance.draft->number) {
          instance.program = instance.draft; instance.updatePending = false; instance.firstDrawSubmittedFrame = 0;
        }
        return finish(requestId, true, "program-update-accepted");
      }
      return finish(requestId, true, "unchanged");
    }
    revision.number = ++revisionClock_;
    // A pending Show names the accepted revision at trigger time. Editing the
    // cue cannot silently retarget that request to a different revision.
    instance.showPending = false;
    instance.draft = std::make_shared<const Revision>(std::move(revision));
    instance.preparation = instance.draft->name.empty() ? "failed" : "unprepared";
    instance.readyRevision = 0;
    instance.warning = instance.draft->name.empty() ? "required-field-empty" : "";
    instance.updatePending = target == "program" && instance.program != nullptr;
    return finish(requestId, true, "revision-accepted");
  }
  if (type == "graphics-instance-cue") {
    if (instance.draft->name.empty()) return finish(requestId, false, "required-field-empty");
    instance.cued = true;
    return finish(requestId, true, "cue-accepted");
  }
  if (type == "graphics-instance-trigger") {
    const auto action = body->getString("action");
    if (action == "show") {
      if (instance.draft->name.empty()) return finish(requestId, false, "required-field-empty");
      if (instance.readyRevision == instance.draft->number) {
        if (instance.program != instance.draft) instance.firstDrawSubmittedFrame = 0;
        show(instance);
      } else instance.showPending = true;
      return finish(requestId, true, "show-accepted");
    }
    if (action != "hide" && action != "clear") return finish(requestId, false, "invalid-trigger");
    instance.showPending = instance.updatePending = false;
    if (action == "clear") {
      instance.program.reset(); instance.timeline.clear(); instance.visibility = "hidden";
      instance.firstDrawSubmittedFrame = 0;
    } else if (instance.program) {
      instance.timeline.hide(instance.program->exit, monotonicMs_());
      advanceAnimations();
    }
    return finish(requestId, true, instance.program ? "hide-accepted" : "hidden");
  }
  if (type == "graphics-instance-remove") {
    if (instance.program || instance.showPending) return finish(requestId, false, "instance-visible");
    instances_.erase(found);
    return finish(requestId, true, "removed");
  }
  return finish(requestId, false, "unsupported-graphics-command");
}

modules::CompositorRenderPlanLayer GraphicsInstances::layer(const Instance& instance, const Revision& revision) const {
  modules::CompositorRenderPlanLayer layer;
  layer.layerId = "graphics:" + instance.id;
  layer.kind = "overlay"; layer.hasOverlayContent = true;
  layer.graphicsInstanceId = instance.id; layer.graphicsRevision = revision.number;
  layer.sourceId = revision.sourceId;
  layer.overlay.title = revision.name; layer.overlay.org = revision.title;
  layer.overlay.appearance = revision.appearance;
  layer.overlay.keyPosition = revision.appearance.anchor;
  const auto& a = revision.appearance;
  const float h = float(std::min(.4, (a.nameSize + a.titleSize + a.padding * 3) / 1080.));
  const float w = float(std::min(a.width, 1. - a.safeOffsetX));
  layer.rect = {a.anchor.find("right") != std::string::npos ? 1.f - float(a.safeOffsetX) - w : float(a.safeOffsetX),
      a.anchor.find("upper") != std::string::npos ? float(a.safeOffsetY) : 1.f - float(a.safeOffsetY) - h, w, h};
  return layer;
}

void GraphicsInstances::append(modules::CompositorRenderPlan& plan, bool preview) const {
  const auto now = monotonicMs_();
  for (const auto& [_, instance] : instances_) {
    // Preparation is private even when the cue belongs to another scene.
    // Program rendering returns readiness; Preview never needs to mutate core.
    if (!preview && !instance.draft->name.empty() && (instance.cued || instance.showPending || instance.updatePending))
      plan.graphicsPreparations.push_back(layer(instance, *instance.draft));
    if (instance.scope == "scene" && instance.sceneId != plan.sceneId) continue;
    const auto revision = preview ? (instance.cued && instance.readyRevision == instance.draft->number ? instance.draft : nullptr)
                                  : instance.program;
    if (!revision) continue;
    if (!revision->sourceId.empty() && std::none_of(plan.layers.begin(), plan.layers.end(), [&](const auto& video) {
      return !video.hasOverlayContent && video.opacity > 0 &&
          (video.kind == "participant-video" || video.kind == "screen-share" || video.kind == "media-video") &&
          (video.sourceId == revision->sourceId || video.participantId == revision->sourceId);
    })) continue;
    auto content = layer(instance, *revision);
    if (!preview) {
      if (instance.timeline.phase(now) == "hidden") continue;
      const auto motion = instance.timeline.sample(now);
      content.opacity = static_cast<float>(motion.alpha);
      content.rect.x += static_cast<float>(motion.x) * content.rect.width;
      content.rect.y += static_cast<float>(motion.y) * content.rect.height;
    }
    content.order = static_cast<int>(plan.layers.size());
    plan.layers.push_back(std::move(content));
  }
}

void GraphicsInstances::observe(const modules::ProgramFrame& frame) {
  bool changed = advanceAnimations();
  for (const auto& fact : frame.graphicsFacts) {
    auto found = instances_.find(fact.instanceId);
    if (found == instances_.end()) continue;
    auto& instance = found->second;
    if (fact.state == "draw-submitted") {
      if (instance.program && instance.program->number == fact.revision && instance.firstDrawSubmittedFrame == 0) {
        instance.firstDrawSubmittedFrame = frame.frameNumber; changed = true;
      }
      continue;
    }
    if (instance.draft->number != fact.revision) continue;
    if (instance.preparation != fact.state || instance.warning != fact.reason) changed = true;
    instance.preparation = fact.state; instance.warning = fact.reason;
    if (fact.state != "ready") continue;
    instance.readyRevision = fact.revision;
    if (instance.showPending) {
      show(instance); changed = true;
    } else if (instance.updatePending) {
      instance.program = instance.draft; instance.firstDrawSubmittedFrame = 0;
      instance.updatePending = false; changed = true;
    }
  }
  if (changed) publish();
}

void GraphicsInstances::show(Instance& instance) {
  if (instance.program != instance.draft) instance.firstDrawSubmittedFrame = 0;
  instance.program = instance.draft;
  instance.timeline.show(instance.program->entrance, monotonicMs_());
  instance.visibility = instance.timeline.phase(monotonicMs_());
  instance.showPending = instance.updatePending = false;
}

bool GraphicsInstances::advanceAnimations() {
  bool changed = false;
  const auto now = monotonicMs_();
  for (auto& [_, instance] : instances_) {
    const auto phase = instance.timeline.phase(now);
    if (phase != instance.visibility) { instance.visibility = phase; changed = true; }
    if (phase == "hidden" && instance.program) {
      instance.program.reset(); instance.updatePending = false;
      instance.firstDrawSubmittedFrame = 0; changed = true;
    }
  }
  return changed;
}

bool GraphicsInstances::hasCue() const {
  return std::any_of(instances_.begin(), instances_.end(), [](const auto& item) { return item.second.cued; });
}

void GraphicsInstances::publish() {
  Json::Array rows;
  for (const auto& [_, instance] : instances_) {
    const auto& a = instance.draft->appearance;
    const contracts::GraphicsRevisionIdentity identity{instance.id, epoch_, static_cast<int64_t>(instance.draft->number)};
    const auto firstComposition = instance.program && instance.firstDrawSubmittedFrame > 0
        ? contracts::toJson(contracts::GraphicsCompositionFact{
            {instance.id, epoch_, static_cast<int64_t>(instance.program->number)}, instance.firstDrawSubmittedFrame, "draw-submitted"})
        : Json{};
    rows.emplace_back(Json::Object{
      {"identity", contracts::toJson(identity)},
      {"preparationObservation", contracts::toJson(contracts::GraphicsPreparationFact{identity, instance.preparation, instance.warning})},
      {"firstCompositionObservation", firstComposition},
      {"instanceId", instance.id}, {"templateId", instance.templateId}, {"templateVersion", 1},
      {"scope", instance.scope}, {"sceneId", instance.sceneId},
      {"revision", double(instance.draft->number)}, {"programRevision", instance.program ? double(instance.program->number) : 0.},
      {"preparation", instance.preparation}, {"cued", instance.cued}, {"showPending", instance.showPending},
      {"visibility", instance.program ? (instance.visibility == "on-air" && instance.firstDrawSubmittedFrame == 0 ? "entering" : instance.visibility) : "hidden"},
      {"firstDrawSubmittedFrame", double(instance.firstDrawSubmittedFrame)},
      {"warning", instance.warning}, {"sourceId", instance.draft->sourceId},
      {"fields", Json::Object{{"name", instance.draft->name}, {"title", instance.draft->title}}},
      {"style", styleState(a)},
      {"animations", Json::Object{{"in", instance.draft->entrance.json()}, {"out", instance.draft->exit.json()}}},
      {"programAnimations", instance.program ? Json{Json::Object{{"in", instance.program->entrance.json()}, {"out", instance.program->exit.json()}}} : Json{}},
      {"programSourceId", instance.program ? instance.program->sourceId : ""},
      {"programFields", instance.program ? Json{Json::Object{{"name", instance.program->name}, {"title", instance.program->title}}} : Json{}},
      {"programStyle", instance.program ? styleState(instance.program->appearance) : Json{}}});
  }
  snapshot_ = Json::Object{{"schemaVersion", 1}, {"coreEpoch", epoch_}, {"instances", std::move(rows)}, {"lastResult", lastResult_}};
}
}  // namespace corevideo::graphics
