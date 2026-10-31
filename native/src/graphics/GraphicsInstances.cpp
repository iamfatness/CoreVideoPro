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
#include "graphics/GraphicsTemplateDraft.h"
#include "graphics/GraphicsCompatibility.h"

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
bool sameDefinition(const std::shared_ptr<const GraphicsTemplate>& a,const std::shared_ptr<const GraphicsTemplate>& b) {
  if(a==b) return true;
  if(!a || !b || a->hash()!=b->hash() || a->assets().size()!=b->assets().size()) return false;
  for(const auto& entry:a->assets()) {const auto found=b->assets().find(entry.first);if(found==b->assets().end() || found->second->sha256!=entry.second->sha256) return false;}
  return true;
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
  if (!draft.isObject() || draft.asObject().size() > 8) return {};
  for (const auto& [key, _] : draft.asObject())
    if (key != "templateId" && key != "templateVersion" && key != "fields" && key != "style" && key != "animations" && key != "placement" && key != "scratch" && key != "image") return {};
  const auto templateId = draft.getString("templateId");
  const auto version = draft.getNumber("templateVersion", 0);
  if (!std::isfinite(version) || std::floor(version) != version || version < 1 || version > 2147483647.) return {};
  const bool compatibility = templateId == "compact-solid" || templateId == "minimal-accent" || templateId == "broadcast";
  auto found = templatesPublication_->definitions.find({templateId, int(version)});
  if ((!compatibility && found == templatesPublication_->definitions.end()) || (compatibility && version != 1)) return {};
  Revision revision; revision.appearance=defaultAppearance(templateId);
  if (!compatibility) {
    revision.definition = found->second; revision.templateFields = revision.definition->defaultFields(); revision.placement = revision.definition->defaultPlacement(); revision.placement = revision.definition->defaultPlacement();
    revision.appearance.backgroundOpacity = 1;
  }
  if (!readRevision(draft, revision) || !revision.fieldsReady) return {};
  hashRevision(templateId, revision);
  if(revision.definition) {
    const auto* scratch=draft.get("scratch"); std::string manifest,expected;
    if(scratch && !scratch->isNull()) {
      if(!scratch->isObject() || scratch->asObject().size()!=2 || !scratch->get("manifest") || !scratch->get("manifest")->isString() || !scratch->get("hash") || !scratch->get("hash")->isString()) return {};
      manifest=scratch->getString("manifest");expected=scratch->getString("hash");
    } else {
      manifest=revision.definition->document().stringify(); static constexpr char hex[]="0123456789abcdef";
      for(auto byte:revision.definition->hash()) {expected+=hex[byte>>4];expected+=hex[byte&15];}
    }
    if(!authoringCompiler_) authoringCompiler_=std::make_shared<GraphicsTemplateDraft>();
    std::string key;
    if(!authoringCompiler_->submit(std::move(manifest),expected,revision.definition,key,revision.templateFields,revision.templateInput->brand)) return {};
    authoringKey_=key;
    authoringInput_=Json::Object{{"templateId",templateId},{"templateVersion",int(version)},{"manifestHash",expected},{"fields",revision.templateFields},
      {"style",draft.get("style") ? *draft.get("style") : Json{Json::Object{}}}};
    auto input=std::make_shared<GraphicsTemplateInput>(); input->definition=revision.definition; input->logo=revision.image;
    input->fields=revision.templateInput->fields; input->brand=revision.templateInput->brand; input->placement=revision.placement;
    input->draft=authoringCompiler_; input->draftKey=key; revision.templateInput=std::move(input);
    if(scratch && !scratch->isNull()) {
      auto identity=std::string(reinterpret_cast<const char*>(revision.contentHash.data()),revision.contentHash.size())+key;
      revision.contentHash=modules::hashing::sha256(reinterpret_cast<const uint8_t*>(identity.data()),identity.size());
    }
  }
  Instance placeholder; placeholder.id = "authoring";
  auto result = layer(placeholder, revision, 1920, 1080);
  result.overlay.graphicsAuthoringRect = result.rect;
  return result.overlay;
}

bool GraphicsInstances::readRevision(const Json& request, Revision& revision) {
  if(const auto* image=request.get("image")) {
    if(image->isNull()) {revision.image.reset();revision.imageReference=Json{};}
    else {
      if(!image->isObject() || image->asObject().size()!=3 ||
          (revision.definition && !revision.definition->assets().count("logo"))) return false;
      const auto version=image->getNumber("templateVersion");
      if(version<1 || version>std::numeric_limits<int>::max() || std::floor(version)!=version) return false;
      const auto package=templatesPublication_->definitions.find({image->getString("templateId"),int(version)});
      if(package==templatesPublication_->definitions.end()) return false;
      const auto asset=package->second->assets().find(image->getString("assetId"));
      if(asset==package->second->assets().end()) return false;
      revision.image=asset->second;revision.imageReference=*image;
    }
  }
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
  if(revision.definition && !revision.definition->validateFields(revision.templateFields,false,error)) return false;
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
      {"availableAssets", std::move(availableAssets)},{"imageHash",revision.image?revision.image->sha256:""},
      {"templateFields", revision.templateFields}}}.stringify();
  revision.contentHash = modules::hashing::sha256(reinterpret_cast<const uint8_t*>(serialized.data()), serialized.size());
  if (revision.definition) {
    const auto& a = revision.appearance;
    const auto alpha = unsigned(std::lround(a.backgroundOpacity * 255)); static constexpr char hex[] = "0123456789ABCDEF";
    const auto background = a.backgroundColor + hex[alpha >> 4] + hex[alpha & 15];
    auto input = std::make_shared<GraphicsTemplateInput>(); input->definition = revision.definition; input->logo = revision.image; input->fields = revision.templateFields; input->placement=revision.placement;
    input->brand = Json::Object{{"brand.primary", background}, {"brand.onPrimary", a.nameColor},
        {"brand.onPrimaryMuted", a.titleColor}, {"brand.fontBody", a.fontFamily},
        {"brand.secondary", a.titleColor}, {"brand.accent", a.accentColor},
        {"brand.background", a.backgroundColor}, {"brand.foreground", a.nameColor}, {"brand.fontHeading", a.fontFamily},
        {"brand.logo", revision.definition->assets().count("logo") ? "logo" : ""}};
    revision.templateInput = std::move(input);
  }
}

bool GraphicsInstances::revisionCapacity(const std::string& id,const Revision& candidate) const {
  std::set<std::array<uint8_t,32>> keys;keys.insert(candidate.contentHash);
  for(const auto& [instanceId,instance]:instances_) {
    if(instanceId!=id) keys.insert(instance.draft->contentHash);
    for(const auto& revision:{instance.cue,instance.pendingCue,instance.program,instance.clockProgramPending}) if(revision) keys.insert(revision->contentHash);
  }
  return keys.size()<=16;
}

bool GraphicsInstances::nodeCapacity(const std::string& id,const Revision& candidate) const {
  const auto count=[](const Revision& revision) -> size_t {
    if(!revision.definition) return 8; // conservative legacy primitive bound
    size_t total=0;
    const auto visit=[&](const auto& self,const Json& layers)->void {
      for(const auto& layer:layers.asArray()) {++total;if(const auto* children=layer.get("children")) self(self,*children);}
    };
    visit(visit,*revision.definition->document().get("layers"));return total;
  };
  size_t total=0;bool found=false;
  for(const auto& [instanceId,instance]:instances_) {
    size_t maximum=count(*instance.draft);
    for(const auto& revision:{instance.cue,instance.pendingCue,instance.program}) if(revision) maximum=std::max(maximum,count(*revision));
    if(instanceId==id) {maximum=std::max(maximum,count(candidate));found=true;}
    total+=maximum;
  }
  if(!found) total+=count(candidate);
  return total<=256;
}

bool GraphicsInstances::finish(const std::string& id, bool accepted, const std::string& reason) {
  lastResult_ = Json::Object{{"clientId",pendingClient_},{"requestId", id}, {"accepted", accepted}, {"reason", reason}};
  if (const auto found = instances_.find(pendingInstance_); found != instances_.end()) {
    auto result = lastResult_.asObject(); result["instanceId"] = found->first;
    result["currentRevision"] = double(found->second.draft->number);
    lastResult_ = std::move(result);
  }
  if (!pendingFingerprint_.empty()) {
    operations_.push_back({pendingClient_, id, std::move(pendingFingerprint_), lastResult_,monotonicMs_()});
    if (operations_.size() > 256) operations_.pop_front();
  }
  publish();
  return accepted;
}

bool GraphicsInstances::apply(const Json& command) {
  updateTemplates();
  updateClocks();
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
      {"graphics-instance-create", {"templateId", "templateVersion", "scope", "sceneId", "sourceId", "fields", "style", "animations", "binding", "placement", "image"}},
      {"graphics-instance-update", {"label", "rackOrder", "templateVersion", "sourceId", "fields", "style", "target", "animations", "binding", "placement", "image"}},
      {"graphics-instance-cue", {"rideTake"}}, {"graphics-instance-trigger", {"action", "target"}},
      {"graphics-clock-control", {"action","configuration","expectedClockRevision"}},
      {"graphics-instance-remove", {}}, {"graphics-program-clear", {"channel"}},
      {"graphics-automation-configure", {"configuration", "expectedAutomationRevision"}}, {"graphics-automation-control", {"action", "mode", "expectedAutomationRevision", "expectedActivationRevision"}},
      {"graphics-authority-set", {"family", "authority"}},
      {"graphics-template-install", {"manifest", "packageRoot"}}, {"graphics-template-list", {"includeThumbnails"}}};
  const auto allowed = actionKeys.find(type);
  if (allowed == actionKeys.end()) return finish(requestId, false, "unsupported-graphics-command");
  for (const auto& [key, value] : body->asObject()) {
    if (!common.count(key) && !allowed->second.count(key)) return finish(requestId, false, "unsupported-request-field");
    if(key=="includeThumbnails") {if(!value.isBool()) return finish(requestId,false,"invalid-thumbnail-demand");}
    else if(key=="rideTake") {if(!value.isBool()) return finish(requestId,false,"invalid-ride-take");}
    else if (key == "manifest") {
      if (!value.isString() || value.asString().empty() || value.asString().size() > 256 * 1024)
        return finish(requestId, false, "template-manifest-size");
    } else if (key == "packageRoot") {
      if (!value.isString() || value.asString().size() > 2048 || value.asString().find('\0') != std::string::npos)
        return finish(requestId, false, "graphics-package-root-invalid");
    } else if(key=="image") {if(!value.isNull() && (!value.isObject() || value.asObject().size()!=3)) return finish(requestId,false,"invalid-graphics-image");}
    else if (key == "configuration") {
      if(type=="graphics-automation-configure") {std::vector<GraphicsAutomation::Rule> rules;Json texts;std::string error;if(!GraphicsAutomation::readConfiguration(value,rules,texts,error)) return finish(requestId,false,error);}
      else {GraphicsClockConfig config;if(!GraphicsClockConfig::read(value,config)) return finish(requestId,false,"invalid-clock-configuration");}
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
    } else if (key == "schemaVersion" || key == "rackOrder" || key == "expectedRevision" || key == "expectedClockRevision" || key == "expectedAutomationRevision" || key == "expectedActivationRevision" || key == "templateVersion") {
      if (!value.isNumber() || !std::isfinite(value.asNumber()) || std::floor(value.asNumber()) != value.asNumber() ||
          value.asNumber() < 0 || value.asNumber() > 9007199254740991.)
        return finish(requestId, false, "invalid-revision");
    } else if (!value.isString() || value.asString().size() > 256) return finish(requestId, false, "invalid-request-value");
  }
  // Install bytes are copied once for bounded retry identity, never parsed,
  // hashed or re-escaped on the frame/control owner.
  while(!operations_.empty() && monotonicMs_()-operations_.front().admittedMs>300000) operations_.pop_front();
  const auto fingerprint = type + ":" + (type == "graphics-template-install" ? body->getString("manifest") + '\0' + body->getString("packageRoot") : body->stringify());
  for (const auto& previous : operations_) if (previous.clientId == pendingClient_ && previous.requestId == requestId) {
    if (previous.fingerprint != fingerprint) return finish(requestId, false, "request-id-reused");
    lastResult_ = previous.result; publish();
    return lastResult_.get("accepted")->asBool();
  }
  pendingFingerprint_ = fingerprint;
  if (type == "graphics-template-list") {
    if(const auto* demand=body->get("includeThumbnails");demand && demand->asBool() && !templates_.requestThumbnails()) return finish(requestId,false,"template-library-busy");
    return finish(requestId, true, "template-catalog-observed");
  }
  if (type == "graphics-template-install") {
    const auto reason = templates_.submit(pendingClient_, requestId, body->getString("manifest"), body->getString("packageRoot"));
    return finish(requestId, reason == "template-install-queued", reason);
  }
  if(type == "graphics-automation-configure") {
    const auto* config=body->get("configuration");if(!config) return finish(requestId,false,"missing-graphics-automation-configuration");
    std::vector<GraphicsAutomation::Rule> rules;Json texts;std::string error;
    if(!GraphicsAutomation::readConfiguration(*config,rules,texts,error)) return finish(requestId,false,error);
    for(const auto& rule:rules) {
      const auto target=instances_.find(rule.instanceId);
      if(target==instances_.end()) return finish(requestId,false,"graphics-automation-target-missing");
      const auto& instance=target->second;
      const auto family=instance.draft->definition?instance.draft->definition->document().getString("family"):"lower-third";
      if(family!="lower-third" || (instance.scope=="scene" && instance.sceneId!=rule.sceneId)) return finish(requestId,false,"graphics-automation-scope-or-family-conflict");
    }
    for(const auto& [_,value]:texts.asObject()) {
      auto binding=GraphicsSourceCatalog::read(*value.get("binding"));
      if(!binding || !sources_->contains(*binding)) return finish(requestId,false,"graphics-automation-source-text-generation-mismatch");
    }
    std::vector<std::string> owned;for(const auto& [id,_]:instances_) if(automation_.owns(id)) owned.push_back(id);
    const auto previousRevision=automation_.revision();
    const bool accepted=automation_.configure(*config,uint64_t(body->getNumber("expectedAutomationRevision",0)),error);
    if(accepted && automation_.revision()!=previousRevision) for(const auto& id:owned) instances_.at(id).showPending=instances_.at(id).updatePending=false;
    return finish(requestId,accepted,accepted?"graphics-automation-configured-unarmed":error);
  }
  if(type == "graphics-automation-control") {
    std::string error;const auto action=body->getString("action");
    if(action=="arm" && !automation_.acceptsArming(uint64_t(body->getNumber("expectedAutomationRevision",0)),uint64_t(body->getNumber("expectedActivationRevision",0)),body->getString("mode"))) return finish(requestId,false,"graphics-automation-arming-conflict");
    if(action=="enable-automatic" && (body->getString("mode")!="automatic" || !automation_.acceptsActivation(uint64_t(body->getNumber("expectedAutomationRevision",0)),uint64_t(body->getNumber("expectedActivationRevision",0))))) return finish(requestId,false,"graphics-automation-arming-conflict");
    const auto requestedId=body->getString("instanceId");
    if(!requestedId.empty() && std::none_of(automation_.rules().begin(),automation_.rules().end(),[&](const auto& rule){return rule.instanceId==requestedId;})) return finish(requestId,false,"graphics-automation-target-missing");
    std::vector<std::string> owned;for(const auto& [id,_]:instances_) if(automation_.owns(id) && (requestedId.empty()||requestedId==id)) owned.push_back(id);
    const bool accepted=automation_.control(action,body->getString("mode"),body->getString("instanceId"),sources_->status()=="available",error);
    if(accepted && (action=="disarm" || action=="hold" || action=="set-mode" || action=="hide-automatic")) {
      for(const auto& id:owned) {
        auto& instance=instances_.at(id);instance.showPending=instance.updatePending=false;
        if(action=="hide-automatic") {instance.timeline.clear();instance.program.reset();instance.programResource.reset();instance.currentlyComposed=false;instance.visibility="hidden";}
      }
      if(action=="hide-automatic") automation_.clear();
    }
    return finish(requestId,accepted,accepted?"graphics-automation-control-accepted":error);
  }
  if(type == "graphics-authority-set") {
    const auto family=body->getString("family"),authority=body->getString("authority");
    if((family!="lower-third" && family!="bug") || (authority!="legacy" && authority!="instances")) return finish(requestId,false,"invalid-graphics-authority");
    if(authority=="instances") managedFamilies_.insert(family);
    else {
      for(auto& [_,instance]:instances_) {
        const auto definition=instance.draft->definition;
        if((definition?definition->document().getString("family"):"lower-third")!=family) continue;
        instance.program.reset();instance.clockProgramPending.reset();instance.programResource.reset();instance.timeline.clear();
        instance.awaitingEntrance=instance.showPending=instance.updatePending=instance.currentlyComposed=false;instance.visibility="hidden";instance.firstDrawSubmittedFrame=0;
      }
      if(family=="lower-third") automation_.clear();
      managedFamilies_.erase(family);
    }
    return finish(requestId,true,"graphics-authority-selected");
  }
  if (type == "graphics-program-clear") {
    if(body->getString("channel")=="all") automation_.clear();
    // This compatibility slice supports an explicit all-graphics clear only.
    // Do not silently interpret a selective channel request as clearing all.
    if (body->getString("channel") != "all") return finish(requestId, false, "unsupported-channel");
    for (auto& [_, instance] : instances_) {
      instance.program.reset(); instance.clockProgramPending.reset(); instance.awaitingEntrance=false; instance.programResource.reset(); instance.showPending = instance.updatePending = false;
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
    instance.id = id; instance.templateId = templateId;instance.label=templateId;instance.rackOrder=int(instances_.size());
    instance.scope = body->getString("scope"); instance.sceneId = body->getString("sceneId");
    if ((instance.scope != "global" && instance.scope != "scene") ||
        (instance.scope == "scene" && (instance.sceneId.empty() || instance.sceneId.size() > 256)) ||
        (instance.scope == "global" && !instance.sceneId.empty()))
      return finish(requestId, false, "invalid-scope");
    Revision revision;
    revision.number = revisionClock_ + 1;
    revision.appearance=defaultAppearance(templateId);
    if (!compatibility) {
      revision.definition = definition->second; revision.templateFields = revision.definition->defaultFields(); revision.placement = revision.definition->defaultPlacement(); revision.placement = revision.definition->defaultPlacement(); revision.appearance.backgroundOpacity = 1;
      if (const auto* animations = revision.definition->document().get("animations"))
        for (const auto& [phase, value] : animations->asObject())
          GraphicsTransition::read(value, phase == "in" ? revision.entrance : revision.exit);
    }
    if (!readRevision(*body, revision)) return finish(requestId, false, "invalid-fields-or-style");
    if (revision.binding && !sources_->contains(*revision.binding)) return finish(requestId, false, "source-identity-changed");
    if(!nodeCapacity(id,revision)) return finish(requestId,false,"graphics-bus-node-capacity");
    hashRevision(templateId, revision);
    if(!revisionCapacity(id,revision)) return finish(requestId,false,"graphics-resident-revision-capacity");
    revisionClock_ = revision.number;
    instance.draft = std::make_shared<const Revision>(std::move(revision));
    if (instance.draft->definition && (instance.draft->definition->document().getString("family")=="clock" || instance.draft->definition->document().getString("family")=="countdown") && instance.draft->templateFields.get("time")) {
      GraphicsClockConfig config;
      if (instance.draft->definition->document().getString("family")=="countdown") config.mode="countdown";
      instance.clockRequestedConfiguration=config.json();
      instance.clock=std::make_shared<GraphicsClockController>(config);
      auto waiting=std::make_shared<Revision>(*instance.draft); waiting->fieldsReady=false; instance.draft=waiting;
      instance.preparation="waiting-clock";
    }
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
  if(type=="graphics-instance-update" && (body->get("label") || body->get("rackOrder"))) {
    static const std::set<std::string> metadataKeys{"schemaVersion","requestId","clientId","coreEpoch","instanceId","expectedRevision","target","label","rackOrder"};
    for(const auto& [key,_]:body->asObject()) if(!metadataKeys.count(key)) return finish(requestId,false,"graphics-rack-metadata-must-be-separate");
    const auto label=body->getString("label",instance.label);const auto order=body->getNumber("rackOrder",instance.rackOrder);
    if(body->getString("target")!="cue" || label.empty() || label.size()>128 || order<0 || order>=instances_.size()) return finish(requestId,false,"invalid-graphics-rack-metadata");
    const auto previous=instance.rackOrder;
    for(auto& [otherId,other]:instances_) if(otherId!=id) {
      if(previous<order && other.rackOrder>previous && other.rackOrder<=order) --other.rackOrder;
      else if(previous>order && other.rackOrder>=order && other.rackOrder<previous) ++other.rackOrder;
    }
    instance.label=label;instance.rackOrder=int(order);return finish(requestId,true,"graphics-rack-metadata-updated");
  }
  if(!automationApplying_ && (type=="graphics-instance-update" || type=="graphics-instance-cue" || type=="graphics-instance-trigger" || type=="graphics-instance-remove")) {
    if(automation_.owns(id)) instance.showPending=instance.updatePending=false;
    automation_.manual(id);
  }
  if (type == "graphics-clock-control") {
    if (!instance.clock) return finish(requestId,false,"instance-has-no-clock");
    const auto action=body->getString("action");
    if (action!="configure" && action!="start" && action!="pause" && action!="resume" && action!="reset") return finish(requestId,false,"invalid-clock-action");
    GraphicsClockConfig config;
    if (action=="configure") {
      if(body->getNumber("expectedClockRevision",-1)!=double(instance.clockConfigurationRevision)) return finish(requestId,false,"clock-configuration-conflict");
      const auto* value=body->get("configuration"); if(!value || !GraphicsClockConfig::read(*value,config)) return finish(requestId,false,"invalid-clock-configuration");
      if(!config.restoreRemainingSeconds && config.json().stringify()==instance.clockRequestedConfiguration.stringify()) return finish(requestId,true,"unchanged");
    }
    else if(body->get("configuration")) return finish(requestId,false,"unexpected-clock-configuration");
    const bool admitted=instance.clock->submit(action,config);
    if(admitted && action=="configure") { ++instance.clockConfigurationRevision; instance.clockRequestedConfiguration=config.json(); }
    return finish(requestId,admitted,admitted ? "clock-control-queued" : "clock-controller-busy");
  }
  if (type == "graphics-instance-update") {
    const auto target = body->getString("target");
    if (target != "cue" && target != "program") return finish(requestId, false, "invalid-target");
    if(instance.clock) if(const auto* fields=body->get("fields")) if(const auto* time=fields->get("time"))
      if(time->stringify()!=instance.draft->templateFields.get("time")->stringify()) return finish(requestId,false,"clock-time-controlled");
    Revision revision = *instance.draft;
    if(const auto* version=body->get("templateVersion")) {
      const auto value=version->asNumber();
      if(value<1 || value>2147483647.) return finish(requestId,false,"template-version-not-installed");
      if(!revision.definition) { if(value!=1) return finish(requestId,false,"template-version-not-installed"); }
      else {
        const auto found=templatesPublication_->definitions.find({instance.templateId,int(value)});
        if(found==templatesPublication_->definitions.end()) return finish(requestId,false,"template-version-not-installed");
        if(found->second->document().getString("family")!=revision.definition->document().getString("family")) return finish(requestId,false,"template-family-change-requires-new-instance");
        revision.definition=found->second;
      }
    }
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
    if (revision.imageReference.stringify()==old.imageReference.stringify() && revision.name == old.name && revision.title == old.title && revision.sourceId == old.sourceId && sameStyle && sameBinding &&
        sameDefinition(revision.definition,old.definition) && revision.placement == old.placement && revision.entrance == old.entrance && revision.exit == old.exit && revision.templateFields.stringify() == old.templateFields.stringify()) {
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
    if(!nodeCapacity(id,revision)) return finish(requestId,false,"graphics-bus-node-capacity");
    hashRevision(instance.templateId, revision);
    if(!revisionCapacity(id,revision)) return finish(requestId,false,"graphics-resident-revision-capacity");
    revision.number = ++revisionClock_;
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
    instance.rideTake = body->get("rideTake") ? body->get("rideTake")->asBool() : true;  // operators ride the Take unless they opt out
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
        clearCue(instance);  // Play In from Preview empties Preview; it does not ride the next Take.
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
    if (action == "uncue") {
      // Preview only: Program and any pending show are untouched.
      const bool wasCued = instance.cued || instance.cue || instance.pendingCue;
      clearCue(instance);
      return finish(requestId, true, wasCued ? "uncued" : "not-cued");
    }
    if (action != "hide" && action != "clear") return finish(requestId, false, "invalid-trigger");
    instance.showPending = instance.updatePending = false;
    if (action == "clear") {
      instance.program.reset(); instance.clockProgramPending.reset(); instance.awaitingEntrance=false; instance.programResource.reset(); instance.timeline.clear(); instance.awaitingEntrance=false; instance.visibility = "hidden";
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
    const auto removedOrder=found->second.rackOrder;instances_.erase(found);
    for(auto& [_,other]:instances_) if(other.rackOrder>removedOrder) --other.rackOrder;
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
  layer.overlay.graphicsImage=revision.image;
  if(revision.image) layer.overlay.imageUri="graphics-asset:"+revision.image->sha256;
  if (revision.binding) {
    layer.graphicsBoundParticipantId = GraphicsSourceCatalog::frameId(revision.binding->sourceId);
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

std::vector<const GraphicsInstances::Instance*> GraphicsInstances::orderedInstances() const {
  std::vector<const Instance*> rows;rows.reserve(instances_.size());for(const auto& [_,instance]:instances_) rows.push_back(&instance);
  std::stable_sort(rows.begin(),rows.end(),[](const auto* a,const auto* b){return a->rackOrder<b->rackOrder;});return rows;
}

void GraphicsInstances::append(modules::CompositorRenderPlan& plan, bool preview,
                               const std::vector<modules::VideoFrame>& frames, ScopePass scope) const {
  if(!preview && automation_.observing()) plan.observeGraphicsSubjects=true;

  plan.layers.erase(std::remove_if(plan.layers.begin(),plan.layers.end(),[&](const auto& item) {
    if(item.layerId.rfind("overlay:",0)!=0) return false;
    return (managedFamilies_.count("lower-third") && (item.layerId=="overlay:lower-third" || item.layerId.ends_with(":lower-third"))) ||
      (managedFamilies_.count("bug") && (item.layerId=="overlay:brand-bug" || item.layerId.ends_with(":bug")));
  }),plan.layers.end());

  const auto now = monotonicMs_();
  bool inserted=false;
  for (const auto* item : orderedInstances()) {
    const auto& instance=*item;
    if (!preview && scope != ScopePass::Scene) {
      // The retained handle validates device/scale on the actual renderer. A
      // ready Cue must remain recoverable even while a different Cue is pending.
      if (instance.cue && (!instance.cue->binding || sources_->contains(*instance.cue->binding))) {
        auto candidate = layer(instance, *instance.cue, plan.width, plan.height);
        candidate.layerId = "accepted-cue:" + instance.id;
        candidate.graphicsPreparedResource = instance.cueResource;
        plan.graphicsPreparations.push_back(std::move(candidate));
      }
      if (instance.clockProgramPending) plan.graphicsPreparations.push_back(layer(instance,*instance.clockProgramPending,plan.width,plan.height));
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
      const auto raw = GraphicsSourceCatalog::frameId(revision->sourceId);
      if (std::any_of(frames.begin(), frames.end(), [&](const auto& video) {
        return video.participantId == raw && video.hasContent() && video.sourceGeneration != uint64_t(revision->binding->generation);
      })) continue;
      const auto frame = std::find_if(frames.begin(), frames.end(), [&](const auto& video) {
        return video.participantId == raw && video.sourceGeneration == uint64_t(revision->binding->generation) && video.hasContent();
      });
      if (frame == frames.end() || std::none_of(plan.layers.begin(), plan.layers.end(), [&](const auto& video) {
        // Match the compositor's explicit frame selector, not a source label
        // that can remain unchanged while a routed participant is replaced.
        return !video.hasOverlayContent && video.opacity > 0 && (video.kind == "participant-video" || video.kind == "screen-share") && video.participantId == raw;
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
    int highest=-1;
    for (const auto& existing:plan.layers) if(!existing.hasOverlayContent || !existing.overlay.isCaption) highest=std::max(highest,existing.order);
    content.order=highest+1;
    plan.layers.push_back(std::move(content)); inserted=true;
  }
  if(inserted) {
    int highest=-1;
    for(const auto& layer:plan.layers) if(!layer.hasOverlayContent || !layer.overlay.isCaption) highest=std::max(highest,layer.order);
    for(auto& layer:plan.layers) if(layer.hasOverlayContent && layer.overlay.isCaption) layer.order=++highest;
  }
}

void GraphicsInstances::observeSources(const std::vector<modules::ZoomEngineParticipant>& roster, uint64_t sessionEpoch,
                                         const std::vector<modules::VideoFrame>& frames,
                                         const std::function<std::string_view(const std::string&)>& captureLabel) {
  automation_.observeEpoch(sessionEpoch);

  if (!sources_->observeSources(roster, sessionEpoch, frames, captureLabel)) return;
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
      instance.updatePending = false; instance.program.reset(); instance.clockProgramPending.reset(); instance.awaitingEntrance=false; instance.programResource.reset(); instance.timeline.clear();
      instance.visibility = "hidden"; instance.firstDrawSubmittedFrame = 0;
    }
  }
  publish();
}

void GraphicsInstances::observe(const modules::ProgramFrame& frame) {
  bool changed = updateTemplates();
  changed = updateAuthoring() || changed;
  changed = updateClocks() || changed;
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
        instance.firstDrawSubmittedFrame = frame.frameNumber;automation_.composed(instance.id,fact.revision,monotonicMs_()); changed = true;
      }
      continue;
    }
    const auto matchesFact=[&](const std::shared_ptr<const Revision>& revision) {
      return revision && (!fact.hasRequestedContentHash || revision->contentHash==fact.requestedContentHash);
    };
    if (instance.clockProgramPending && instance.program && instance.clockProgramPending->number==instance.program->number && instance.clockProgramPending->number==fact.revision && matchesFact(instance.clockProgramPending)) {
      if(fact.state=="ready" && resourceMatches(fact.preparedResource,instance.clockProgramPending->contentHash)) {
        instance.program=instance.clockProgramPending; instance.programResource=fact.preparedResource;
        instance.clockProgramPending.reset(); changed=true;
      } else if(fact.state=="failed") { instance.warning=fact.reason; instance.clockProgramPending.reset(); changed=true; }
    }
    if (fact.state == "ready" && instance.cue && instance.cue->number == fact.revision &&
        resourceMatches(fact.preparedResource, instance.cue->contentHash)) {
      if (instance.cueResource != fact.preparedResource) changed = true;
      instance.cueResource = fact.preparedResource;
    }
    if (instance.pendingCue && instance.pendingCue->number == fact.revision && matchesFact(instance.pendingCue)) {
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
    if (instance.draft->number != fact.revision || !matchesFact(instance.draft)) continue;
    // Clock ticks retain the logical edit revision. A completion for previous
    // digits must never fail or replace the latest requested text.
    if (instance.clock && fact.state=="ready" && !resourceMatches(fact.preparedResource,instance.draft->contentHash)) continue;
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
  changed=applyAutomation(frame)||changed;
  if (changed) publish();
}

bool GraphicsInstances::applyAutomation(const modules::ProgramFrame& frame) {
  if(!automation_.observing() || frame.frameNumber<=0 || !frame.gpuComposed) return false;
  const auto before=automation_.traceSequence();
  std::vector<GraphicsAutomation::Subject> subjects;bool sharing=false;
  for(const auto& fact:frame.graphicsFacts) {
    if(fact.state=="share-draw-submitted") sharing=true;
    if((fact.state!="source-draw-submitted" && fact.state!="share-draw-submitted") || fact.instanceId.rfind("source:",0)!=0 || !fact.revision) continue;
    const auto sourceId=GraphicsSourceCatalog::sourceId(fact.instanceId.substr(7));
    for(const auto& source:sources_->rows().asArray()) {
      const auto* identity=source.get("identity");if(!identity || !source.get("videoOn")->asBool()) continue;
      auto binding=GraphicsSourceCatalog::read(*identity);
      if(binding && binding->sourceId==sourceId && binding->generation==int64_t(fact.revision)) {subjects.push_back({*binding,source.getString("displayName")});break;}
    }
  }
  const auto decisions=automation_.tick(subjects,frame.renderPlanEvidence?frame.renderPlanEvidence->sceneId:"",sharing,monotonicMs_());
  const auto savedResult=lastResult_;
  for(const auto& decision:decisions) {
    auto target=instances_.find(decision.instanceId);if(target==instances_.end()) {automation_.result(decision,false,"graphics-automation-target-missing",0,monotonicMs_());continue;}
    const auto send=[&](const std::string& type,Json::Object body) {
      body["schemaVersion"]=1;body["coreEpoch"]=epoch_;body["clientId"]="native-graphics-rules";body["instanceId"]=decision.instanceId;
      body["requestId"]="rule-"+std::to_string(++automationSequence_);body["expectedRevision"]=double(target->second.draft->number);
      const bool previous=automationApplying_;automationApplying_=true;
      bool accepted=false;try {accepted=apply(Json::Object{{"type",type},{"graphics",std::move(body)}});} catch(...) {automationApplying_=previous;throw;}
      automationApplying_=previous;return accepted;
    };
    bool accepted;
    if(decision.action=="hide") accepted=send("graphics-instance-trigger",{{"action","hide"}});
    else {
      Json::Object fields{{"name",decision.name},{"title",decision.secondary}};
      const auto definition=target->second.draft->definition;
      if(definition) {
        const auto* declared=definition->document().get("fields");
        for(auto field=fields.begin();field!=fields.end();) {
          if(std::none_of(declared->asArray().begin(),declared->asArray().end(),[&](const auto& d){return d.getString("id")==field->first && d.getString("type")=="string" && d.get("bindable") && d.get("bindable")->asBool();})) field=fields.erase(field);else ++field;
        }
        if(!fields.count("name")) {automation_.result(decision,false,"graphics-automation-name-field-not-bindable",target->second.draft->number,monotonicMs_());continue;}
      }
      accepted=send("graphics-instance-update",{{"target",decision.action=="update"?"program":"cue"},{"fields",fields},{"binding",decision.binding?contracts::toJson(*decision.binding):Json{}}});
      if(accepted && decision.action=="cue") accepted=send("graphics-instance-cue",{{"rideTake",false}});  // rehearsal never rides the Take
      else if(accepted && decision.action=="show") accepted=send("graphics-instance-trigger",{{"action","show"}});
    }
    automation_.result(decision,accepted,lastResult_.getString("reason"),target->second.draft->number,monotonicMs_());
  }
  // Rule acknowledgements cannot replace the last external operator result.
  lastResult_=savedResult;
  return before!=automation_.traceSequence() || !decisions.empty();
}

void GraphicsInstances::show(Instance& instance, bool fromCue) {
  const auto revision = fromCue ? instance.cue : instance.draft;
  const bool starting = !instance.program;
  if (instance.program != revision) instance.firstDrawSubmittedFrame = 0;
  instance.clockProgramPending.reset();
  instance.program = revision;
  const auto family=revision->definition?revision->definition->document().getString("family"):"lower-third";
  if(family=="lower-third" || family=="bug") managedFamilies_.insert(family);
  instance.programResource = fromCue ? instance.cueResource : instance.draftResource;
  if (starting && instance.program->entrance.type != "none" && instance.program->entrance.durationMs > 0) instance.awaitingEntrance=true;
  if (!instance.awaitingEntrance) instance.timeline.show(instance.program->entrance, monotonicMs_());
  instance.visibility = instance.awaitingEntrance ? "awaiting-composition" : instance.timeline.phase(monotonicMs_());
  instance.showPending = instance.updatePending = false;
}

void GraphicsInstances::clearCue(Instance& instance) {
  instance.cue.reset(); instance.cueResource.reset(); instance.pendingCue.reset();
  instance.cued = false; instance.cuePreparation = "unprepared"; instance.cueWarning.clear();
}

void GraphicsInstances::promoteCuedForTake() {
  bool changed = false;
  for (auto& [_, instance] : instances_) {
    if (!instance.cued || !instance.rideTake || !instance.cue) continue;
    if (instance.cue->binding && !sources_->contains(*instance.cue->binding)) continue;
    if (!resourceMatches(instance.cueResource, instance.cue->contentHash)) continue;
    show(instance, true);
    clearCue(instance);
    changed = true;
  }
  if (changed) publish();
}

bool GraphicsInstances::updateAuthoring() {
  if(!authoringCompiler_) return false;
  auto publication=authoringCompiler_->poll();
  if(!publication || publication->key!=authoringKey_ || publication==authoringPublication_) return false;
  authoringPublication_=publication;
  authoringState_=Json::Object{{"key",publication->key},{"input",authoringInput_},{"state",publication->definition ? "validated" : "invalid"},{"reason",publication->error},{"layout",publication->layout}};
  return true;
}

bool GraphicsInstances::updateClocks() {
  bool changed=false;
  for (auto& entry:instances_) {
    auto& instance=entry.second; if(!instance.clock) continue;
    auto sample=instance.clock->sample(); if(!sample || sample==instance.clockSample) continue;
    instance.clockSample=sample; changed=true;
    const auto text=sample->getString("text");
    const auto revise=[&](const std::shared_ptr<const Revision>& before) -> std::shared_ptr<const Revision> {
      if(!before || (before->fieldsReady && before->templateFields.getString("time")==text)) return before;
      auto revision=std::make_shared<Revision>(*before); revision->fieldsReady=true; auto fields=revision->templateFields.asObject(); fields["time"]=text;
      revision->templateFields=std::move(fields); hashRevision(instance.templateId,*revision); return revision;
    };
    const auto draft=revise(instance.draft);
    if(draft!=instance.draft) {
      instance.draft=draft; instance.draftResource.reset(); instance.readyRevision=0; instance.preparation="unprepared";
    }
    if(instance.pendingCue) instance.pendingCue=revise(instance.pendingCue);
    else if(instance.cue && instance.cue->templateFields.getString("time")!=text) {
      instance.pendingCue=revise(instance.cue); instance.cuePreparation="unprepared";
    }
    if(instance.program && instance.program->templateFields.getString("time")!=text) instance.clockProgramPending=revise(instance.program);
    if(sample->getString("finishAction")=="hide") {
      instance.program.reset(); instance.clockProgramPending.reset(); instance.programResource.reset(); instance.timeline.clear();
      instance.awaitingEntrance=false; instance.visibility="hidden"; instance.showPending=instance.updatePending=false;
    }
  }
  return changed;
}

bool GraphicsInstances::advanceAnimations() {
  bool changed = false;
  const auto now = monotonicMs_();
  for (auto& [_, instance] : instances_) {
    if (instance.awaitingEntrance && instance.program) continue;
    const auto phase = instance.timeline.phase(now);
    if (phase != instance.visibility) { instance.visibility = phase; changed = true; }
    if (phase == "hidden" && instance.program) {
      instance.program.reset(); instance.clockProgramPending.reset(); instance.awaitingEntrance=false; instance.programResource.reset(); instance.updatePending = false;
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
  for (const auto* item : orderedInstances()) {
    const auto& instance=*item;
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
      {"label",instance.label},{"rackOrder",instance.rackOrder},{"instanceId", instance.id}, {"templateId", instance.templateId}, {"templateVersion", instance.draft->definition ? instance.draft->definition->version() : 1},
      {"scope", instance.scope}, {"sceneId", instance.sceneId},
      {"clock",instance.clockSample ? *instance.clockSample : Json{}},
      {"clockConfigurationRevision",instance.clock ? double(instance.clockConfigurationRevision) : 0.},
      {"clockRequestedConfiguration",instance.clockRequestedConfiguration},
      {"revision", double(instance.draft->number)}, {"programRevision", instance.program ? double(instance.program->number) : 0.},
      {"cueRevision", instance.cue ? double(instance.cue->number) : 0.},
      {"cueTemplateVersion",instance.cue && instance.cue->definition ? instance.cue->definition->version() : 1},
      {"programTemplateVersion",instance.program && instance.program->definition ? instance.program->definition->version() : 1},
      {"cueRequestedRevision", double(instance.cueRequestedRevision)},
      {"cuePending", instance.pendingCue != nullptr}, {"cuePreparation", instance.cuePreparation}, {"cueWarning", instance.cueWarning},
      {"cueFields", instance.cue ? (instance.cue->definition ? instance.cue->templateFields : Json{Json::Object{{"name", instance.cue->name}, {"title", instance.cue->title}}}) : Json{}},
      {"cueStyle", instance.cue ? styleState(instance.cue->appearance) : Json{}},
      {"cueBinding", instance.cue && instance.cue->binding ? contracts::toJson(*instance.cue->binding) : Json{}},
      {"cueAnimations", instance.cue ? Json{Json::Object{{"in", instance.cue->entrance.json()}, {"out", instance.cue->exit.json()}}} : Json{}},
      {"cueResource", resourceState(instance.cueResource)},
      {"preparation", instance.preparation}, {"cued", instance.cued}, {"rideTake", instance.rideTake}, {"showPending", instance.showPending},
      {"preparedResource", resourceState(instance.draftResource)}, {"programResource", resourceState(instance.programResource)},
      {"visibility", instance.program ? (instance.visibility == "on-air" && instance.firstDrawSubmittedFrame == 0 ? "entering" : instance.visibility) : "hidden"},
      {"firstDrawSubmittedFrame", double(instance.firstDrawSubmittedFrame)},
      {"currentlyComposed", instance.currentlyComposed && instance.program != nullptr},
      {"warning", instance.warning}, {"sourceId", instance.draft->sourceId},
      {"binding", instance.draft->binding ? contracts::toJson(*instance.draft->binding) : Json{}},
      {"bindingState", !instance.draft->binding ? "unbound" : sources_->contains(*instance.draft->binding) ? "bound" : "source-identity-changed"},
      {"programBinding", instance.program && instance.program->binding ? contracts::toJson(*instance.program->binding) : Json{}},
      {"image",instance.draft->imageReference},{"cueImage",instance.cue?instance.cue->imageReference:Json{}},{"programImage",instance.program?instance.program->imageReference:Json{}},
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
  Json::Array operations;for(const auto& operation:operations_) operations.push_back(operation.result);
  snapshot_ = Json::Object{{"operations",operations},{"automation",automation_.snapshot()},{"familyAuthorities",Json::Object{{"lower-third",managedFamilies_.count("lower-third")?"instances":"legacy"},{"bug",managedFamilies_.count("bug")?"instances":"legacy"}}},{"schemaVersion", 1}, {"coreEpoch", epoch_}, {"authoring",authoringState_}, {"instances", std::move(rows)},
      {"resourceBudgetAtStatePublication", Json::Object{{"kind", "allocation-model"},{"residentResources",double(budget.residentResources)},{"residentHighWater",double(budget.residentHighWater)},{"residentLimit",double(budget.residentLimit)},
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
