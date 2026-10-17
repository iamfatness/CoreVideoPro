#include "graphics/GraphicsInstances.h"
#include "contracts/Lifecycle.h"
#include <algorithm>
#include <cmath>
#include <limits>
#include <random>
#include <sstream>
#include <set>
#include <chrono>
#include "modules/Sha256.h"
#include "graphics/GraphicsResourceBudget.h"
#include "graphics/GraphicsTemplate.h"

namespace corevideo::graphics {
using rpc::Json;

namespace {
bool resourceMatches(const std::shared_ptr<const GraphicsPreparedResource>& resource,
                     const std::array<uint8_t, 32>& contentHash) {
  if (!resource || !resource->native) return false;
  if (resource->backend == GraphicsPreparedResource::Backend::Test) return true;
  return resource->deviceIdentity && resource->width > 0 && resource->height > 0 &&
      resource->hasContentHash && resource->contentHash == contentHash;
}
Json styleState(const compositor::LowerThirdAppearance& a) {
  return Json::Object{{"version", 1}, {"preset", a.preset}, {"anchor", a.anchor}, {"fontFamily", a.fontFamily},
      {"nameColor", a.nameColor}, {"titleColor", a.titleColor}, {"backgroundColor", a.backgroundColor},
      {"accentColor", a.accentColor}, {"nameSize", a.nameSize}, {"titleSize", a.titleSize},
      {"backgroundOpacity", a.backgroundOpacity}, {"padding", a.padding}, {"width", a.width},
      {"cornerRadius", a.cornerRadius}, {"safeOffsetX", a.safeOffsetX}, {"safeOffsetY", a.safeOffsetY},
      {"logoScale", a.logoScale}, {"showLogo", a.showLogo}};
}
Json resourceState(const std::shared_ptr<const GraphicsPreparedResource>& resource) {
  if (!resource) return {};
  std::string contentKey = std::to_string(resource->signature);
  if (resource->hasContentHash) {
    static constexpr char hex[] = "0123456789abcdef";
    contentKey.clear(); contentKey.reserve(64);
    for (auto byte : resource->contentHash) { contentKey += hex[byte >> 4]; contentKey += hex[byte & 15]; }
  }
  return Json::Object{{"backend", resource->backend == GraphicsPreparedResource::Backend::D3D11 ? "d3d11" :
      resource->backend == GraphicsPreparedResource::Backend::Metal ? "metal" : "test"},
      {"width", resource->width}, {"height", resource->height}, {"contentKey", contentKey}};
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
  sources_.emplace(epoch_);
  updateTemplates();
  publish();
}

bool GraphicsInstances::updateTemplates() {
  auto publication = templates_.poll();
  if (publication == templatesPublication_) return false;
  templatesPublication_ = std::move(publication); return true;
}

bool GraphicsInstances::validId(const std::string& value) {
  if (value.empty() || value.size() > 64 || value[0] < 'a' || value[0] > 'z') return false;
  return std::all_of(value.begin(), value.end(), [](char c) {
    return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-';
  });
}

std::optional<modules::CompositorOverlayContent> GraphicsInstances::authoringPreview(const Json& draft) {
  updateTemplates();
  if (!draft.isObject() || draft.asObject().size() > 6) return {};
  for (const auto& [key, _] : draft.asObject())
    if (key != "templateId" && key != "templateVersion" && key != "fields" && key != "style" && key != "animations" && key != "placement") return {};
  const auto templateId = draft.getString("templateId");
  const auto version = draft.getNumber("templateVersion", 0);
  if (!std::isfinite(version) || std::floor(version) != version || version < 1 || version > 2147483647.) return {};
  const bool compatibility = templateId == "compact-solid" || templateId == "minimal-accent" || templateId == "broadcast";
  auto found = templatesPublication_->definitions.find({templateId, int(version)});
  if ((!compatibility && found == templatesPublication_->definitions.end()) || (compatibility && version != 1)) return {};
  Revision revision; revision.appearance.enabled = true;
  revision.appearance.preset = compatibility ? templateId : "compact-solid";
  if (!compatibility) {
    revision.definition = found->second; revision.templateFields = revision.definition->defaultFields(); revision.placement = revision.definition->defaultPlacement(); revision.placement = revision.definition->defaultPlacement();
    revision.appearance.backgroundOpacity = 1;
  }
  if (!readRevision(draft, revision) || !revision.fieldsReady) return {};
  hashRevision(templateId, revision);
  Instance placeholder; placeholder.id = "authoring";
  auto result = layer(placeholder, revision, 1920, 1080);
  result.overlay.graphicsAuthoringRect = result.rect;
  return result.overlay;
}

bool GraphicsInstances::readRevision(const Json& request, Revision& revision) {
  if (const auto* placement = request.get("placement")) {
    if (!GraphicsPlacement::read(*placement,revision.placement)) return false;
  }
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
    if (revision.definition) {
      auto values = revision.templateFields.asObject(); for (const auto& [key, value] : fields->asObject()) values[key] = value;
      std::string error; Json merged{std::move(values)};
      if (!revision.definition->validateFields(merged, false, error)) return false;
      revision.templateFields = std::move(merged);
      revision.name = revision.templateFields.getString("name"); revision.title = revision.templateFields.getString("title");
    } else for (const auto& [key, value] : fields->asObject()) {
      if (!value.isString()) return false;
      if (key == "name" && value.asString().size() <= 256) revision.name = value.asString();
      else if (key == "title" && value.asString().size() <= 512) revision.title = value.asString();
      else return false;
    }
  }
  if (const auto* style = request.get("style")) {
    if (!style->isObject()) return false;
    if (revision.definition) for (const auto& [key, _] : style->asObject()) {
      static const std::set<std::string> supported{"version", "anchor", "fontFamily", "nameColor", "titleColor", "backgroundColor", "accentColor", "backgroundOpacity", "safeOffsetX", "safeOffsetY"};
      if (!supported.count(key)) return false;
    }
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
    if (revision.sourceId.empty()) revision.binding.reset();
  }
  if (const auto* binding = request.get("binding")) {
    if (binding->isNull()) {
      if (!request.getString("sourceId").empty()) return false;
      revision.binding.reset(); revision.sourceId.clear();
    } else {
      const auto identity = GraphicsSourceCatalog::read(*binding);
      if (!identity || (request.get("sourceId") && request.getString("sourceId") != identity->sourceId)) return false;
      revision.binding = *identity; revision.sourceId = identity->sourceId;
    }
  }
  std::string error;
  revision.fieldsReady = revision.definition ? revision.definition->validateFields(revision.templateFields, true, error) : !revision.name.empty();
  return revision.sourceId.empty() || (revision.binding && revision.binding->sourceId == revision.sourceId);
}

void GraphicsInstances::hashRevision(const std::string& templateId, Revision& revision) {
  std::string definitionHash;
  Json::Array availableAssets;
  if (revision.definition) {
    static constexpr char hex[] = "0123456789abcdef";
    for (const auto byte : revision.definition->hash()) { definitionHash += hex[byte >> 4]; definitionHash += hex[byte & 15]; }
    for (const auto& asset : revision.definition->assets()) availableAssets.emplace_back(asset.first + ":" + asset.second->sha256);
  }
  const auto serialized = Json{Json::Object{{"templateId", templateId}, {"templateVersion", revision.definition ? revision.definition->version() : 1},
      {"name", revision.name}, {"title", revision.title}, {"style", styleState(revision.appearance)},
      {"binding", revision.binding ? contracts::toJson(*revision.binding) : Json{}},
      {"definitionHash", definitionHash},
      {"availableAssets", std::move(availableAssets)},
      {"templateFields", revision.templateFields}}}.stringify();
  revision.contentHash = modules::hashing::sha256(reinterpret_cast<const uint8_t*>(serialized.data()), serialized.size());
  if (revision.definition) {
    const auto& a = revision.appearance;
    const auto alpha = unsigned(std::lround(a.backgroundOpacity * 255)); static constexpr char hex[] = "0123456789ABCDEF";
    const auto background = a.backgroundColor + hex[alpha >> 4] + hex[alpha & 15];
    auto input = std::make_shared<GraphicsTemplateInput>(); input->definition = revision.definition; input->fields = revision.templateFields;
    input->brand = Json::Object{{"brand.primary", background}, {"brand.onPrimary", a.nameColor},
        {"brand.onPrimaryMuted", a.titleColor}, {"brand.fontBody", a.fontFamily},
        {"brand.secondary", a.titleColor}, {"brand.accent", a.accentColor},
        {"brand.background", a.backgroundColor}, {"brand.foreground", a.nameColor}, {"brand.fontHeading", a.fontFamily},
        {"brand.logo", revision.definition->assets().count("logo") ? "logo" : ""}};
    revision.templateInput = std::move(input);
  }
}

bool GraphicsInstances::finish(const std::string& id, bool accepted, const std::string& reason) {
  lastResult_ = Json::Object{{"requestId", id}, {"accepted", accepted}, {"reason", reason}};
  if (const auto found = instances_.find(pendingInstance_); found != instances_.end()) {
    auto result = lastResult_.asObject(); result["instanceId"] = found->first;
    result["currentRevision"] = double(found->second.draft->number);
    lastResult_ = std::move(result);
  }
  if (!pendingFingerprint_.empty()) {
    operations_.push_back({pendingClient_, id, std::move(pendingFingerprint_), lastResult_});
    if (operations_.size() > 32) operations_.pop_front();
  }
  publish();
  return accepted;
}

bool GraphicsInstances::apply(const Json& command) {
  updateTemplates();
  advanceAnimations();
  pendingFingerprint_.clear(); pendingClient_.clear(); pendingInstance_.clear();
  const auto type = command.getString("type");
  const auto* body = command.get("graphics");
  if (!body || !body->isObject()) return finish("", false, "missing-graphics-request");
  const auto requestId = body->getString("requestId");
  if (!contracts::validateGraphicsOperationIdentity(*body) || !validId(requestId) || body->getNumber("schemaVersion", 0) != 1)
    return finish(requestId.substr(0, 64), false, "invalid-graphics-request");
  if (body->getString("coreEpoch") != epoch_) return finish(requestId, false, "core-epoch-conflict");
  pendingClient_ = body->getString("clientId");
  pendingInstance_ = body->getString("instanceId");
  if (!validId(pendingClient_)) return finish(requestId, false, "invalid-client-id");
  // Bound before serializing a dedup fingerprint, and refuse unsupported knobs.
  static const std::set<std::string> common{"schemaVersion", "requestId", "clientId", "coreEpoch", "instanceId", "expectedRevision"};
  static const std::map<std::string, std::set<std::string>> actionKeys{
      {"graphics-instance-create", {"templateId", "templateVersion", "scope", "sceneId", "sourceId", "fields", "style", "animations", "binding", "placement"}},
      {"graphics-instance-update", {"sourceId", "fields", "style", "target", "animations", "binding", "placement"}},
      {"graphics-instance-cue", {}}, {"graphics-instance-trigger", {"action", "target"}},
      {"graphics-instance-remove", {}}, {"graphics-program-clear", {"channel"}},
      {"graphics-template-install", {"manifest", "packageRoot"}}, {"graphics-template-list", {}}};
  const auto allowed = actionKeys.find(type);
  if (allowed == actionKeys.end()) return finish(requestId, false, "unsupported-graphics-command");
  for (const auto& [key, value] : body->asObject()) {
    if (!common.count(key) && !allowed->second.count(key)) return finish(requestId, false, "unsupported-request-field");
    if (key == "manifest") {
      if (!value.isString() || value.asString().empty() || value.asString().size() > 256 * 1024)
        return finish(requestId, false, "template-manifest-size");
    } else if (key == "packageRoot") {
      if (!value.isString() || value.asString().size() > 2048 || value.asString().find('\0') != std::string::npos)
        return finish(requestId, false, "graphics-package-root-invalid");
    } else if (key == "placement") {
      std::optional<GraphicsPlacement> parsed; if (!GraphicsPlacement::read(value,parsed)) return finish(requestId,false,"invalid-placement");
    } else if (key == "binding") {
      if (!value.isNull() && !GraphicsSourceCatalog::read(value)) return finish(requestId, false, "invalid-source-binding");
    } else if (key == "animations") {
      if (!value.isObject() || value.asObject().size() > 2) return finish(requestId, false, "invalid-animations");
      for (const auto& [phase, transition] : value.asObject()) {
        GraphicsTransition parsed;
        if ((phase != "in" && phase != "out") || !GraphicsTransition::read(transition, parsed))
          return finish(requestId, false, "invalid-animations");
      }
    } else if (key == "fields" || key == "style") {
      if (!value.isObject() || value.asObject().size() > (key == "fields" ? 32 : 24)) return finish(requestId, false, "invalid-fields-or-style");
      size_t bytes = 0;
      for (const auto& [field, item] : value.asObject())
        if ((bytes += field.size() + (item.isString() ? item.asString().size() : 32)) > 32768 || field.size() > 64 ||
            (item.isString() ? item.asString().size() > (key == "fields" ? 4096 : 512) :
            !(item.isBool() || (item.isNumber() && std::isfinite(item.asNumber())))))
          return finish(requestId, false, "invalid-fields-or-style");
    } else if (key == "schemaVersion" || key == "expectedRevision" || key == "templateVersion") {
      if (!value.isNumber() || !std::isfinite(value.asNumber()) || std::floor(value.asNumber()) != value.asNumber() ||
          value.asNumber() < 0 || value.asNumber() > 9007199254740991.)
        return finish(requestId, false, "invalid-revision");
    } else if (!value.isString() || value.asString().size() > 256) return finish(requestId, false, "invalid-request-value");
  }
  // Install bytes are copied once for bounded retry identity, never parsed,
  // hashed or re-escaped on the frame/control owner.
  const auto fingerprint = type + ":" + (type == "graphics-template-install" ? body->getString("manifest") + '\0' + body->getString("packageRoot") : body->stringify());
  for (const auto& previous : operations_) if (previous.clientId == pendingClient_ && previous.requestId == requestId) {
    if (previous.fingerprint != fingerprint) return finish(requestId, false, "request-id-reused");
    lastResult_ = previous.result; publish();
    return lastResult_.get("accepted")->asBool();
  }
  pendingFingerprint_ = fingerprint;
  if (type == "graphics-template-list") return finish(requestId, true, "template-catalog-observed");
  if (type == "graphics-template-install") {
    const auto reason = templates_.submit(pendingClient_, requestId, body->getString("manifest"), body->getString("packageRoot"));
    return finish(requestId, reason == "template-install-queued", reason);
  }
  if (type == "graphics-program-clear") {
    // This compatibility slice supports an explicit all-graphics clear only.
    // Do not silently interpret a selective channel request as clearing all.
    if (body->getString("channel") != "all") return finish(requestId, false, "unsupported-channel");
    for (auto& [_, instance] : instances_) {
      instance.program.reset(); instance.awaitingEntrance=false; instance.programResource.reset(); instance.showPending = instance.updatePending = false;
      instance.firstDrawSubmittedFrame = 0;
      instance.timeline.clear(); instance.awaitingEntrance=false; instance.visibility = "hidden";
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
    const auto version = body->getNumber("templateVersion", 0);
    if (version < 1 || version > std::numeric_limits<int>::max()) return finish(requestId, false, "template-version-not-installed");
    const bool compatibility = templateId == "compact-solid" || templateId == "minimal-accent" || templateId == "broadcast";
    auto definition = templatesPublication_->definitions.find({templateId, int(version)});
    if (!compatibility && definition == templatesPublication_->definitions.end()) return finish(requestId, false, "template-version-not-installed");
    if (compatibility && version != 1) return finish(requestId, false, "template-version-not-installed");
    Instance instance;
    instance.id = id; instance.templateId = templateId;
    instance.scope = body->getString("scope"); instance.sceneId = body->getString("sceneId");
    if ((instance.scope != "global" && instance.scope != "scene") ||
        (instance.scope == "scene" && (instance.sceneId.empty() || instance.sceneId.size() > 256)) ||
        (instance.scope == "global" && !instance.sceneId.empty()))
      return finish(requestId, false, "invalid-scope");
    Revision revision;
    revision.number = revisionClock_ + 1;
    revision.appearance.enabled = true; revision.appearance.preset = compatibility ? templateId : "compact-solid";
    if (!compatibility) {
      revision.definition = definition->second; revision.templateFields = revision.definition->defaultFields(); revision.placement = revision.definition->defaultPlacement(); revision.placement = revision.definition->defaultPlacement(); revision.appearance.backgroundOpacity = 1;
      if (const auto* animations = revision.definition->document().get("animations"))
        for (const auto& [phase, value] : animations->asObject())
          GraphicsTransition::read(value, phase == "in" ? revision.entrance : revision.exit);
    }
    if (templateId == "minimal-accent") {
      revision.appearance.backgroundOpacity = 0; revision.appearance.nameSize = 40;
      revision.appearance.titleSize = 26; revision.appearance.cornerRadius = 0;
    } else if (templateId == "broadcast") {
      revision.appearance.nameSize = 52; revision.appearance.titleSize = 32;
      revision.appearance.padding = 24; revision.appearance.width = .75;
      revision.appearance.cornerRadius = 0; revision.appearance.backgroundOpacity = 1;
    }
    if (!readRevision(*body, revision)) return finish(requestId, false, "invalid-fields-or-style");
    if (revision.binding && !sources_->contains(*revision.binding)) return finish(requestId, false, "source-identity-changed");
    hashRevision(templateId, revision);
    revisionClock_ = revision.number;
    instance.draft = std::make_shared<const Revision>(std::move(revision));
    instances_.emplace(id, std::move(instance));
    return finish(requestId, true, "created-hidden");
  }
  if (found == instances_.end()) return finish(requestId, false, "instance-not-found");
  auto& instance = found->second;
  const auto triggerTarget = type == "graphics-instance-trigger" ? body->getString("target") : "";
  if (type == "graphics-instance-trigger" && body->get("target") && (triggerTarget != "cue" || body->getString("action") != "show"))
    return finish(requestId, false, "invalid-trigger-target");
  if (triggerTarget == "cue" && !instance.cue) return finish(requestId, false, "cue-not-ready");
  const double expected = body->getNumber("expectedRevision", -1);
  const auto expectedIdentity = triggerTarget == "cue" ? instance.cue : instance.draft;
  if (expected != static_cast<double>(expectedIdentity->number)) return finish(requestId, false, "revision-conflict");
  if (type == "graphics-instance-update") {
    const auto target = body->getString("target");
    if (target != "cue" && target != "program") return finish(requestId, false, "invalid-target");
    Revision revision = *instance.draft;
    if (!readRevision(*body, revision)) return finish(requestId, false, "invalid-fields-or-style");
    if (revision.binding && !sources_->contains(*revision.binding)) return finish(requestId, false, "source-identity-changed");
    if (target == "program" && !revision.fieldsReady) return finish(requestId, false, "required-field-empty");
    // Preserve explicit blanks. Same values must not allocate/raster or replay.
    const auto& old = *instance.draft;
    const auto& a = revision.appearance; const auto& b = old.appearance;
    const bool sameStyle = a.preset == b.preset && a.anchor == b.anchor && a.fontFamily == b.fontFamily &&
        a.nameColor == b.nameColor && a.titleColor == b.titleColor && a.backgroundColor == b.backgroundColor &&
        a.accentColor == b.accentColor && a.nameSize == b.nameSize && a.titleSize == b.titleSize &&
        a.backgroundOpacity == b.backgroundOpacity && a.padding == b.padding && a.width == b.width &&
        a.cornerRadius == b.cornerRadius && a.safeOffsetX == b.safeOffsetX && a.safeOffsetY == b.safeOffsetY &&
        a.logoScale == b.logoScale && a.showLogo == b.showLogo;
    const bool sameBinding = revision.binding.has_value() == old.binding.has_value() &&
        (!revision.binding || GraphicsSourceCatalog::same(*revision.binding, *old.binding));
    if (revision.name == old.name && revision.title == old.title && revision.sourceId == old.sourceId && sameStyle && sameBinding &&
        revision.placement == old.placement && revision.entrance == old.entrance && revision.exit == old.exit && revision.templateFields.stringify() == old.templateFields.stringify()) {
      if (target == "program" && instance.program && instance.program != instance.draft) {
        instance.updatePending = true;
        if (instance.readyRevision == instance.draft->number) {
          instance.program = instance.draft; instance.programResource = instance.draftResource;
          instance.updatePending = false; instance.firstDrawSubmittedFrame = 0;
        }
        return finish(requestId, true, "program-update-accepted");
      }
      return finish(requestId, true, "unchanged");
    }
    revision.number = ++revisionClock_;
    hashRevision(instance.templateId, revision);
    // A pending Show names the accepted revision at trigger time. Editing the
    // cue cannot silently retarget that request to a different revision.
    instance.showPending = false;
    instance.draft = std::make_shared<const Revision>(std::move(revision));
    instance.preparation = instance.draft->fieldsReady ? "unprepared" : "failed";
    instance.draftResource.reset();
    instance.readyRevision = 0;
    instance.warning = instance.draft->fieldsReady ? "" : "required-field-empty";
    instance.updatePending = target == "program" && instance.program != nullptr;
    return finish(requestId, true, "revision-accepted");
  }
  if (type == "graphics-instance-cue") {
    if (instance.draft->binding && !sources_->contains(*instance.draft->binding)) return finish(requestId, false, "source-identity-changed");
    if (!instance.draft->fieldsReady) return finish(requestId, false, "required-field-empty");
    if (instance.preparation == "failed") return finish(requestId, false, "graphics-preparation-failed");
    instance.cued = true;
    instance.cueRequestedRevision = instance.draft->number;
    instance.cueWarning.clear();
    if (instance.readyRevision == instance.draft->number && instance.draftResource) {
      instance.cue = instance.draft; instance.cueResource = instance.draftResource;
      instance.pendingCue.reset(); instance.cuePreparation = "ready";
    } else {
      instance.pendingCue = instance.draft; instance.cuePreparation = "preparing";
    }
    return finish(requestId, true, "cue-accepted");
  }
  if (type == "graphics-instance-trigger") {
    const auto action = body->getString("action");
    if (action == "show") {
      if (triggerTarget == "cue") {
        if (instance.cue->binding && !sources_->contains(*instance.cue->binding)) return finish(requestId, false, "source-identity-changed");
        if (!resourceMatches(instance.cueResource, instance.cue->contentHash)) return finish(requestId, false, "cue-not-ready");
        show(instance, true);
        return finish(requestId, true, "cue-show-accepted");
      }
      if (instance.draft->binding && !sources_->contains(*instance.draft->binding)) return finish(requestId, false, "source-identity-changed");
      if (!instance.draft->fieldsReady) return finish(requestId, false, "required-field-empty");
      if (instance.preparation == "failed") return finish(requestId, false, "graphics-preparation-failed");
      if (instance.readyRevision == instance.draft->number) {
        if (instance.program != instance.draft) instance.firstDrawSubmittedFrame = 0;
        show(instance);
      } else instance.showPending = true;
      return finish(requestId, true, "show-accepted");
    }
    if (action != "hide" && action != "clear") return finish(requestId, false, "invalid-trigger");
    instance.showPending = instance.updatePending = false;
    if (action == "clear") {
      instance.program.reset(); instance.awaitingEntrance=false; instance.programResource.reset(); instance.timeline.clear(); instance.awaitingEntrance=false; instance.visibility = "hidden";
      instance.firstDrawSubmittedFrame = 0;
    } else if (instance.program) {
      instance.awaitingEntrance=false;
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

modules::CompositorRenderPlanLayer GraphicsInstances::layer(const Instance& instance, const Revision& revision, int width, int height) const {
  modules::CompositorRenderPlanLayer layer;
  layer.layerId = "graphics:" + instance.id;
  layer.kind = "overlay"; layer.hasOverlayContent = true;
  layer.graphicsInstanceId = instance.id; layer.graphicsRevision = revision.number;
  layer.sourceId = revision.sourceId;
  layer.overlay.title = revision.name; layer.overlay.org = revision.title;
  layer.overlay.appearance = revision.appearance;
  layer.overlay.graphicsContentHash = revision.contentHash;
  layer.overlay.hasGraphicsContentHash = true;
  layer.overlay.graphicsTemplate = revision.templateInput;
  if (revision.binding) {
    layer.graphicsBoundParticipantId = revision.binding->sourceId.substr(5);
    layer.graphicsBoundSourceGeneration = uint64_t(revision.binding->generation);
  }
  layer.overlay.keyPosition = revision.appearance.anchor;
  const auto& a = revision.appearance;
  const float h = float(revision.definition ? revision.definition->height() / 1080. : std::min(.4, (a.nameSize + a.titleSize + a.padding * 3) / 1080.));
  const float w = float(revision.definition ? revision.definition->width() * std::max(height, 1) / (1080. * std::max(width, 1)) : std::min(a.width, 1. - a.safeOffsetX));
  layer.rect = {a.anchor.find("right") != std::string::npos ? 1.f - float(a.safeOffsetX) - w : float(a.safeOffsetX),
      a.anchor.find("upper") != std::string::npos ? float(a.safeOffsetY) : 1.f - float(a.safeOffsetY) - h, w, h};
  if (revision.placement) {
    const auto& p=*revision.placement;
    const float x=p.anchor.find("right")!=std::string::npos?1.f:p.anchor.find("left")!=std::string::npos?0.f:.5f;
    const float y=p.anchor.find("upper")!=std::string::npos?0.f:p.anchor.find("lower")!=std::string::npos?1.f:.5f;
    layer.rect.width*=float(p.scale); layer.rect.height*=float(p.scale);
    layer.rect.x=x*(1-layer.rect.width)+float(p.x/1920.);
    layer.rect.y=y*(1-layer.rect.height)+float(p.y/1080.);
  }
  return layer;
}

void GraphicsInstances::append(modules::CompositorRenderPlan& plan, bool preview,
                               const std::vector<modules::VideoFrame>& frames, ScopePass scope) const {
  const auto now = monotonicMs_();
  std::stable_partition(plan.layers.begin(), plan.layers.end(), [](const auto& l) { return !l.hasOverlayContent || !l.overlay.isCaption; });
  for (const auto& [_, instance] : instances_) {
    if (!preview && scope != ScopePass::Scene) {
      // The retained handle validates device/scale on the actual renderer. A
      // ready Cue must remain recoverable even while a different Cue is pending.
      if (instance.cue && (!instance.cue->binding || sources_->contains(*instance.cue->binding))) {
        auto candidate = layer(instance, *instance.cue, plan.width, plan.height);
        candidate.layerId = "accepted-cue:" + instance.id;
        candidate.graphicsPreparedResource = instance.cueResource;
        plan.graphicsPreparations.push_back(std::move(candidate));
      }
      if (instance.pendingCue && instance.cuePreparation != "failed" &&
          (!instance.pendingCue->binding || sources_->contains(*instance.pendingCue->binding))) {
        auto candidate = layer(instance, *instance.pendingCue, plan.width, plan.height);
        candidate.layerId = "pending-cue:" + instance.id;
        plan.graphicsPreparations.push_back(std::move(candidate));
      }
    }
    // Preparation is private even when the cue belongs to another scene.
    // Program rendering returns readiness; Preview never needs to mutate core.
    if (!preview && scope != ScopePass::Scene && instance.pendingCue != instance.draft && instance.cue != instance.draft &&
        instance.draft->fieldsReady && (!instance.draft->binding || sources_->contains(*instance.draft->binding)) &&
        (instance.cued || instance.showPending || instance.updatePending)) {
      const auto candidate = layer(instance, *instance.draft, plan.width, plan.height);
      const int width = int(std::lround(double(candidate.rect.width) * plan.width)), height = int(std::lround(double(candidate.rect.height) * plan.height));
      if (!instance.draftResource || instance.draftResource->width != width || instance.draftResource->height != height)
        plan.graphicsPreparations.push_back(candidate);
    }
    if ((scope == ScopePass::Scene && instance.scope != "scene") ||
        (scope == ScopePass::Global && instance.scope != "global")) continue;
    if (instance.scope == "scene" && instance.sceneId != plan.sceneId) continue;
    const auto revision = preview ? instance.cue : instance.program;
    if (!revision) continue;
    if (revision->binding) {
      if (!sources_->contains(*revision->binding)) continue;
      const auto raw = revision->sourceId.substr(5); // catalog currently qualifies Zoom camera sources only
      if (std::any_of(frames.begin(), frames.end(), [&](const auto& video) {
        return video.participantId == raw && video.hasContent() && video.sourceGeneration != uint64_t(revision->binding->generation);
      })) continue;
      const auto frame = std::find_if(frames.begin(), frames.end(), [&](const auto& video) {
        return video.participantId == raw && video.sourceGeneration == uint64_t(revision->binding->generation) && video.hasContent();
      });
      if (frame == frames.end() || std::none_of(plan.layers.begin(), plan.layers.end(), [&](const auto& video) {
        // Match the compositor's explicit frame selector, not a source label
        // that can remain unchanged while a routed participant is replaced.
        return !video.hasOverlayContent && video.opacity > 0 && video.kind == "participant-video" && video.participantId == raw;
      })) continue;
    }
    auto content = layer(instance, *revision, plan.width, plan.height);
    content.graphicsPreparedResource = preview ? instance.cueResource : instance.programResource;
    if (!preview) {
      content.graphicsAwaitingEntrance = instance.awaitingEntrance;
      if (!instance.awaitingEntrance) {
        if (instance.timeline.phase(now) == "hidden") continue;
        const auto motion = instance.timeline.sample(now);
        content.opacity = static_cast<float>(motion.alpha);
        content.rect.x += static_cast<float>(motion.x) * content.rect.width;
        content.rect.y += static_cast<float>(motion.y) * content.rect.height;
      }
    }
    // Captions are downstream of graphics. Existing scene transition order can
    // contain large offsets, so a vector size is not a safe z-order.
    const auto caption = std::find_if(plan.layers.begin(), plan.layers.end(), [](const auto& l) { return l.hasOverlayContent && l.overlay.isCaption; });
    plan.layers.insert(caption, std::move(content));
  }
  for (size_t index = 0; index < plan.layers.size(); ++index) plan.layers[index].order = int(index);
}

void GraphicsInstances::observeZoomSources(const std::vector<modules::ZoomEngineParticipant>& roster, uint64_t sessionEpoch,
                                         const std::vector<modules::VideoFrame>& frames) {
  if (!sources_->observeZoom(roster, sessionEpoch, frames)) return;
  for (auto& [_, instance] : instances_) {
    if (instance.pendingCue && instance.pendingCue->binding && !sources_->contains(*instance.pendingCue->binding)) {
      instance.pendingCue.reset(); instance.cuePreparation = "failed"; instance.cueWarning = "source-identity-changed";
    }
    if (instance.cue && instance.cue->binding && !sources_->contains(*instance.cue->binding)) {
      instance.cue.reset(); instance.cueResource.reset();
      if (!instance.pendingCue) { instance.cuePreparation = "failed"; instance.cueWarning = "source-identity-changed"; }
    }
    const bool draftValid = !instance.draft->binding || sources_->contains(*instance.draft->binding);
    if (!draftValid) {
      instance.showPending = instance.updatePending = false;
      instance.draftResource.reset(); instance.readyRevision = 0; instance.preparation = "unprepared";
    }
    if (instance.program && instance.program->binding && !sources_->contains(*instance.program->binding)) {
      // Retire an obsolete subject before composing the changed video. A prior
      // explicit update to a valid replacement remains an explicit reveal intent.
      instance.showPending = instance.showPending || (instance.updatePending && draftValid);
      instance.updatePending = false; instance.program.reset(); instance.awaitingEntrance=false; instance.programResource.reset(); instance.timeline.clear();
      instance.visibility = "hidden"; instance.firstDrawSubmittedFrame = 0;
    }
  }
  publish();
}

void GraphicsInstances::observe(const modules::ProgramFrame& frame) {
  bool changed = updateTemplates();
  changed = advanceAnimations() || changed;
  if (frame.frameNumber > 0) for (auto& entry : instances_) {
    auto& instance=entry.second;
    const bool composed=instance.program && std::any_of(frame.graphicsFacts.begin(),frame.graphicsFacts.end(),[&](const auto& fact) {
      return fact.instanceId==entry.first && fact.revision==instance.program->number && fact.state=="draw-submitted";
    });
    if (composed!=instance.currentlyComposed) { instance.currentlyComposed=composed; changed=true; }
  }
  for (const auto& fact : frame.graphicsFacts) {
    auto found = instances_.find(fact.instanceId);
    if (found == instances_.end()) continue;
    auto& instance = found->second;
    if (fact.state == "composition-eligible") {
      if (instance.awaitingEntrance && instance.program && instance.program->number == fact.revision) {
        instance.awaitingEntrance=false; instance.timeline.show(instance.program->entrance,monotonicMs_());
        instance.visibility=instance.timeline.phase(monotonicMs_()); changed=true;
      }
      continue;
    }
    if (fact.state == "draw-submitted") {
      if (instance.program && instance.program->number == fact.revision && instance.firstDrawSubmittedFrame == 0) {
        instance.firstDrawSubmittedFrame = frame.frameNumber; changed = true;
      }
      continue;
    }
    if (fact.state == "ready" && instance.cue && instance.cue->number == fact.revision &&
        resourceMatches(fact.preparedResource, instance.cue->contentHash)) {
      if (instance.cueResource != fact.preparedResource) changed = true;
      instance.cueResource = fact.preparedResource;
    }
    if (instance.pendingCue && instance.pendingCue->number == fact.revision) {
      const bool ready = fact.state == "ready" && resourceMatches(fact.preparedResource, instance.pendingCue->contentHash);
      const auto state = fact.state == "ready" && !ready ? "failed" : fact.state;
      const auto warning = fact.state == "ready" && !ready ? "Graphics preparation reported ready without a matching resource." : fact.reason;
      if (instance.cuePreparation != state || instance.cueWarning != warning) changed = true;
      instance.cuePreparation = state; instance.cueWarning = warning;
      if (ready) {
        instance.cue = instance.pendingCue; instance.cueResource = fact.preparedResource;
        instance.pendingCue.reset(); changed = true;
      } else if (state == "failed") { instance.pendingCue.reset(); changed = true; }
    }
    if (fact.state == "ready" && instance.program && instance.program->number == fact.revision &&
        resourceMatches(fact.preparedResource, instance.program->contentHash)) {
      if (instance.programResource != fact.preparedResource) changed = true;
      instance.programResource = fact.preparedResource;
    }
    if (instance.draft->number != fact.revision) continue;
    if (instance.draft->binding && !sources_->contains(*instance.draft->binding)) continue;
    if (fact.state == "ready" && !resourceMatches(fact.preparedResource, instance.draft->contentHash)) {
      instance.preparation = "failed"; instance.warning = "Graphics preparation reported ready without a matching resource.";
      instance.showPending = instance.updatePending = false; changed = true; continue;
    }
    if (instance.preparation != fact.state || instance.warning != fact.reason) changed = true;
    instance.preparation = fact.state; instance.warning = fact.reason;
    if (fact.state == "failed") instance.showPending = instance.updatePending = false;
    if (fact.state != "ready") continue;
    instance.readyRevision = fact.revision;
    if (instance.draftResource != fact.preparedResource) changed = true;
    instance.draftResource = fact.preparedResource;
    if (instance.showPending) {
      show(instance); changed = true;
    } else if (instance.updatePending) {
      instance.program = instance.draft; instance.programResource = instance.draftResource; instance.firstDrawSubmittedFrame = 0;
      instance.updatePending = false; changed = true;
    }
  }
  if (changed) publish();
}

void GraphicsInstances::show(Instance& instance, bool fromCue) {
  const auto revision = fromCue ? instance.cue : instance.draft;
  const bool starting = !instance.program;
  if (instance.program != revision) instance.firstDrawSubmittedFrame = 0;
  instance.program = revision;
  instance.programResource = fromCue ? instance.cueResource : instance.draftResource;
  if (starting && instance.program->entrance.type != "none" && instance.program->entrance.durationMs > 0) instance.awaitingEntrance=true;
  if (!instance.awaitingEntrance) instance.timeline.show(instance.program->entrance, monotonicMs_());
  instance.visibility = instance.awaitingEntrance ? "awaiting-composition" : instance.timeline.phase(monotonicMs_());
  instance.showPending = instance.updatePending = false;
}

bool GraphicsInstances::advanceAnimations() {
  bool changed = false;
  const auto now = monotonicMs_();
  for (auto& [_, instance] : instances_) {
    if (instance.awaitingEntrance && instance.program) continue;
    const auto phase = instance.timeline.phase(now);
    if (phase != instance.visibility) { instance.visibility = phase; changed = true; }
    if (phase == "hidden" && instance.program) {
      instance.program.reset(); instance.awaitingEntrance=false; instance.programResource.reset(); instance.updatePending = false;
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
      {"instanceId", instance.id}, {"templateId", instance.templateId}, {"templateVersion", instance.draft->definition ? instance.draft->definition->version() : 1},
      {"scope", instance.scope}, {"sceneId", instance.sceneId},
      {"revision", double(instance.draft->number)}, {"programRevision", instance.program ? double(instance.program->number) : 0.},
      {"cueRevision", instance.cue ? double(instance.cue->number) : 0.},
      {"cueRequestedRevision", double(instance.cueRequestedRevision)},
      {"cuePending", instance.pendingCue != nullptr}, {"cuePreparation", instance.cuePreparation}, {"cueWarning", instance.cueWarning},
      {"cueFields", instance.cue ? (instance.cue->definition ? instance.cue->templateFields : Json{Json::Object{{"name", instance.cue->name}, {"title", instance.cue->title}}}) : Json{}},
      {"cueStyle", instance.cue ? styleState(instance.cue->appearance) : Json{}},
      {"cueBinding", instance.cue && instance.cue->binding ? contracts::toJson(*instance.cue->binding) : Json{}},
      {"cueAnimations", instance.cue ? Json{Json::Object{{"in", instance.cue->entrance.json()}, {"out", instance.cue->exit.json()}}} : Json{}},
      {"cueResource", resourceState(instance.cueResource)},
      {"preparation", instance.preparation}, {"cued", instance.cued}, {"showPending", instance.showPending},
      {"preparedResource", resourceState(instance.draftResource)}, {"programResource", resourceState(instance.programResource)},
      {"visibility", instance.program ? (instance.visibility == "on-air" && instance.firstDrawSubmittedFrame == 0 ? "entering" : instance.visibility) : "hidden"},
      {"firstDrawSubmittedFrame", double(instance.firstDrawSubmittedFrame)},
      {"currentlyComposed", instance.currentlyComposed && instance.program != nullptr},
      {"warning", instance.warning}, {"sourceId", instance.draft->sourceId},
      {"binding", instance.draft->binding ? contracts::toJson(*instance.draft->binding) : Json{}},
      {"bindingState", !instance.draft->binding ? "unbound" : sources_->contains(*instance.draft->binding) ? "bound" : "source-identity-changed"},
      {"programBinding", instance.program && instance.program->binding ? contracts::toJson(*instance.program->binding) : Json{}},
      {"fields", instance.draft->definition ? instance.draft->templateFields : Json{Json::Object{{"name", instance.draft->name}, {"title", instance.draft->title}}}},
      {"style", styleState(a)},
      {"placement",instance.draft->placement ? instance.draft->placement->json() : Json{}},
      {"programPlacement",instance.program && instance.program->placement ? instance.program->placement->json() : Json{}},
      {"cuePlacement",instance.cue && instance.cue->placement ? instance.cue->placement->json() : Json{}},
      {"animations", Json::Object{{"in", instance.draft->entrance.json()}, {"out", instance.draft->exit.json()}}},
      {"programAnimations", instance.program ? Json{Json::Object{{"in", instance.program->entrance.json()}, {"out", instance.program->exit.json()}}} : Json{}},
      {"programSourceId", instance.program ? instance.program->sourceId : ""},
      {"programFields", instance.program ? (instance.program->definition ? instance.program->templateFields : Json{Json::Object{{"name", instance.program->name}, {"title", instance.program->title}}}) : Json{}},
      {"programStyle", instance.program ? styleState(instance.program->appearance) : Json{}}});
  }
  const auto budget = GraphicsResourceBudget::process().counters();
  snapshot_ = Json::Object{{"schemaVersion", 1}, {"coreEpoch", epoch_}, {"instances", std::move(rows)},
      {"resourceBudgetAtStatePublication", Json::Object{{"kind", "allocation-model"},
          {"cpuBytes", double(budget.cpuBytes)}, {"gpuBytes", double(budget.gpuBytes)},
          {"activePreparations", double(budget.activePreparations)}, {"cpuHighWater", double(budget.cpuHighWater)},
          {"gpuHighWater", double(budget.gpuHighWater)}, {"preparationHighWater", double(budget.preparationHighWater)},
          {"refused", double(budget.refused)}, {"cpuLimit", double(budget.cpuLimit)},
          {"gpuLimit", double(budget.gpuLimit)}, {"preparationLimit", double(budget.preparationLimit)}}},
      {"templates", templatesPublication_->catalog}, {"templateInstalls", templatesPublication_->installs},
      {"encodedTemplateAssetBytes", double(templatesPublication_->encodedAssetBytes)}, {"encodedTemplateAssetLimit", 128 * 1024 * 1024},
      {"sources", sources_->rows()}, {"sourceBindingStatus", sources_->status()}, {"lastResult", lastResult_}};
}
}  // namespace corevideo::graphics
