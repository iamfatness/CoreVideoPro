#include "graphics/GraphicsAutomation.h"
#include "graphics/GraphicsSourceCatalog.h"
#include <algorithm>
#include <cmath>

namespace corevideo::graphics {
using rpc::Json;
namespace {
bool id(const std::string& value) {return !value.empty() && value.size()<=64 && std::all_of(value.begin(),value.end(),[](char c){return (c>='a'&&c<='z')||(c>='0'&&c<='9')||c=='-';});}
bool bounded(const Json& object,const std::string& key,double fallback,double maximum,double& value) {const auto* raw=object.get(key);value=raw?raw->asNumber(-1):fallback;return (!raw||raw->isNumber()) && std::isfinite(value) && value>=0 && value<=maximum;}
std::string key(const contracts::SourceInstanceIdentity& source) {return source.processEpoch+":"+source.instanceId+":"+std::to_string(source.generation);}
}
bool GraphicsAutomation::readConfiguration(const Json& document,std::vector<Rule>& rules,Json& texts,std::string& error) {
  error="invalid-graphics-automation-configuration";
  if(!document.isObject() || document.asObject().size()>2) return false;
  for(const auto& p:document.asObject()) if(p.first!="rules" && p.first!="sourceTexts") return false;
  const auto* entries=document.get("rules");if(!entries || !entries->isArray() || entries->asArray().size()>8) return false;
  std::set<std::string> ids,targets;rules.clear();
  for(const auto& entry:entries->asArray()) {
    if(!entry.isObject() || entry.asObject().size()>10) return false;
    static const std::set<std::string> allowed{"id","instanceId","enabled","sceneId","sourceId","dwellMs","durationMs","cooldownMs","suppressScreenShare"};
    for(const auto& p:entry.asObject()) if(!allowed.count(p.first)) return false;
    Rule rule;rule.id=entry.getString("id");rule.instanceId=entry.getString("instanceId");rule.sceneId=entry.getString("sceneId");rule.sourceId=entry.getString("sourceId");rule.document=entry;
    for(const auto* name:{"sceneId","sourceId"}) if(const auto* value=entry.get(name);value && !value->isString()) return false;
    if(!id(rule.id)||!id(rule.instanceId)||!ids.insert(rule.id).second||!targets.insert(rule.instanceId).second||rule.sceneId.size()>256||rule.sourceId.size()>256) return false;
    for(const auto* name:{"enabled","suppressScreenShare"}) if(const auto* value=entry.get(name);value && !value->isBool()) return false;
    rule.enabled=entry.get("enabled")?entry.get("enabled")->asBool():true;rule.suppressScreenShare=entry.get("suppressScreenShare")?entry.get("suppressScreenShare")->asBool():true;
    if(!bounded(entry,"dwellMs",1500,60000,rule.dwellMs)||!bounded(entry,"durationMs",8000,3600000,rule.durationMs)||!bounded(entry,"cooldownMs",0,3600000,rule.cooldownMs)) return false;
    rules.push_back(std::move(rule));
  }
  texts=document.get("sourceTexts")?*document.get("sourceTexts"):Json::Object{};
  if(!texts.isObject() || texts.asObject().size()>32 || texts.stringify().size()>32768) return false;
  for(const auto& [source,values]:texts.asObject()) {
    if(source.empty() || source.size()>256 || !values.isObject() || values.asObject().size()>3) return false;
    const auto* rawBinding=values.get("binding");
    auto binding=rawBinding?GraphicsSourceCatalog::read(*rawBinding):std::nullopt;
    if(!binding || binding->sourceId!=source) return false;
    for(const auto& [name,value]:values.asObject()) {
      if(name=="binding") continue;
      if((name!="name"&&name!="secondary") || !value.isString() || value.asString().size()>(name=="name"?256u:512u) || (name=="name"&&value.asString().empty())) return false;
    }
  }
  error.clear();return true;
}
bool GraphicsAutomation::configure(const Json& config,uint64_t expected,std::string& error) {
  if(expected!=revision_) {error="graphics-automation-revision-conflict";return false;}
  std::vector<Rule> rules;Json texts;if(!readConfiguration(config,rules,texts,error)) return false;
  Json::Array previous,next;for(const auto& rule:rules_) previous.push_back(rule.document);for(const auto& rule:rules) next.push_back(rule.document);
  if(Json(previous).stringify()==Json(next).stringify() && texts.stringify()==sourceTexts_.stringify()) {error.clear();return true;}
  // Saving a rule is not activation. Never reset emergency/manual inhibition
  // through import/configuration, and never replay a formerly pending action.
  std::map<std::string,State> retained;std::set<std::string> holds;
  for(const auto& rule:rules) {
    const auto previous=std::find_if(rules_.begin(),rules_.end(),[&](const auto& old){return old.id==rule.id && old.instanceId==rule.instanceId;});
    if(previous!=rules_.end()) if(const auto state=states_.find(rule.id);state!=states_.end()) retained.emplace(rule.id,state->second);
    if(manualHolds_.count(rule.instanceId)) holds.insert(rule.instanceId);
  }
  suggestions_.clear();states_=std::move(retained);manualHolds_=std::move(holds);rules_=std::move(rules);sourceTexts_=std::move(texts);armed_=false;++revision_;return true;
}
bool GraphicsAutomation::control(const std::string& action,const std::string& mode,const std::string& instanceId,bool available,std::string& error) {
  if(action=="set-mode") {
    if(mode!="manual"&&mode!="suggest"&&mode!="rehearsal"&&mode!="automatic") {error="invalid-graphics-automation-mode";return false;}
    if(mode_==mode) {error.clear();return true;}
    mode_=mode;armed_=false;suggestions_.clear();
    // Suggestions/rehearsal are not automatic reveals. An explicit mode
    // transition begins a new qualification cycle, never an implicit Show.
    for(auto& entry:states_) {entry.second.fired.clear();entry.second.lastFired=-1;}
  } else if(action=="arm") {
    if((mode_!="automatic"&&mode_!="rehearsal") || !available || rules_.empty()) {error="graphics-automation-not-qualified";return false;}
    if(armed_&&!inhibited_) {error.clear();return true;}
    armed_=true;inhibited_=false;
  } else if(action=="disarm" || action=="hide-automatic") armed_=false;
  else if(action=="hold") {if(instanceId.empty()) globalHold_=true;else manual(instanceId);}
  else if(action=="resume") {if(instanceId.empty()) {manualHolds_.clear();globalHold_=false;}else manualHolds_.erase(instanceId);}
  else {error="invalid-graphics-automation-action";return false;}
  // A fresh dwell is required after every explicit mode/arming transition.
  for(auto& entry:states_) {entry.second.candidate.clear();entry.second.lastNotice.clear();}
  error.clear();return true;
}
void GraphicsAutomation::manual(const std::string& instanceId) {
  if(std::none_of(rules_.begin(),rules_.end(),[&](const auto& rule){return rule.instanceId==instanceId;})) return;
  forgetSuggestion(instanceId);
  if(manualHolds_.size()<8) manualHolds_.insert(instanceId);
  for(const auto& rule:rules_) if(rule.instanceId==instanceId) {auto& state=states_[rule.id];state.owned=false;state.candidate.clear();state.lastNotice.clear();}
}
bool GraphicsAutomation::owns(const std::string& id) const {
  for(const auto& rule:rules_) if(rule.instanceId==id) {const auto found=states_.find(rule.id);if(found!=states_.end()&&found->second.owned) return true;}return false;
}
void GraphicsAutomation::clear() {suggestions_.clear();armed_=false;inhibited_=true;for(auto& entry:states_) {entry.second.owned=false;entry.second.candidate.clear();entry.second.lastNotice.clear();}}
void GraphicsAutomation::observeEpoch(uint64_t epoch) {if(sourceEpoch_==epoch) return;const bool reconnect=sourceEpoch_!=0;sourceEpoch_=epoch;if(reconnect) {clear();states_.clear();}}
void GraphicsAutomation::forgetSuggestion(const std::string& id) {if(suggestions_.erase(id)) ++suggestionVersion_;}
void GraphicsAutomation::suggest(const Decision& value) {
 const auto found=suggestions_.find(value.instanceId);
 if(found!=suggestions_.end() && found->second.name==value.name && found->second.secondary==value.secondary && found->second.binding && value.binding && GraphicsSourceCatalog::same(*found->second.binding,*value.binding)) return;
 suggestions_[value.instanceId]=value;++suggestionVersion_;
}
void GraphicsAutomation::trace(const Rule& rule,State& state,const std::string& status,const std::string& reason,const std::string& subject,uint64_t revision) {
  const auto notice=status+":"+reason+":"+subject+":"+std::to_string(revision);if(state.lastNotice==notice) return;state.lastNotice=notice;
  if(trace_.size()==256) {trace_.erase(trace_.begin());++droppedTrace_;}
  trace_.emplace_back(Json::Object{{"sequence",double(++traceSequence_)},{"ruleId",rule.id},{"instanceId",rule.instanceId},{"state",status},{"reason",reason},{"subject",subject},{"revision",double(revision)}});
}
std::vector<GraphicsAutomation::Decision> GraphicsAutomation::tick(const std::vector<Subject>& subjects,const std::string& sceneId,bool sharing,double now) {
  std::vector<Decision> decisions;if(mode_=="manual") return decisions;
  for(const auto& rule:rules_) {
    auto& state=states_[rule.id];
    if(!rule.enabled || inhibited_ || globalHold_ || manualHolds_.count(rule.instanceId) || ((mode_=="automatic"||mode_=="rehearsal")&&!armed_)) {
      forgetSuggestion(rule.instanceId);state.candidate.clear();trace(rule,state,"blocked",inhibited_?"clear-inhibited":globalHold_||manualHolds_.count(rule.instanceId)?"manual-hold":!rule.enabled?"rule-disabled":"not-armed");continue;
    }
    const Subject* subject=nullptr;bool ambiguous=false;
    if((rule.sceneId.empty()||rule.sceneId==sceneId) && !(sharing&&rule.suppressScreenShare)) for(const auto& candidate:subjects) {
      if(!rule.sourceId.empty() && candidate.identity.sourceId!=rule.sourceId) continue;
      if(subject && key(subject->identity)!=key(candidate.identity)) {ambiguous=true;break;}subject=&candidate;
    }
    if(ambiguous) subject=nullptr;
    if(!subject) {
      forgetSuggestion(rule.instanceId);state.candidate.clear();
      if(mode_=="automatic" && state.owned) {decisions.push_back({rule.id,rule.instanceId,"hide",{},{},{}});state.owned=false;}
      trace(rule,state,"blocked",sharing&&rule.suppressScreenShare?"screen-share-suppressed":ambiguous?"ambiguous-rendered-subject":"no-qualified-rendered-subject");continue;
    }
    const auto identity=key(subject->identity);
    std::string name=subject->name,secondary;
    if(const auto* values=sourceTexts_.get(subject->identity.sourceId)) {
      const auto* rawBinding=values->get("binding");auto binding=rawBinding?GraphicsSourceCatalog::read(*rawBinding):std::nullopt;
      if(binding && GraphicsSourceCatalog::same(*binding,subject->identity)) {name=values->getString("name",name);secondary=values->getString("secondary");}
    }
    const auto copy=name+'\0'+secondary;
    if(state.owned && state.ownedSubject!=identity && mode_=="automatic") {decisions.push_back({rule.id,rule.instanceId,"hide",{},{},{}});state.owned=false;}
    if(state.owned && state.firstDraw>=0 && rule.durationMs>0 && now-state.firstDraw>=rule.durationMs && mode_=="automatic") {decisions.push_back({rule.id,rule.instanceId,"hide",{},{},{}});state.owned=false;trace(rule,state,"requested","auto-hide",identity);continue;}
    if(state.fired.count(identity)) {
      if(mode_=="suggest") suggest({rule.id,rule.instanceId,"cue",name,secondary,subject->identity});
      if(state.owned && state.lastCopy!=copy && mode_=="automatic") {state.lastCopy=copy;decisions.push_back({rule.id,rule.instanceId,"update",name,secondary,subject->identity});}
      continue;
    }
    if(state.candidate!=identity) {forgetSuggestion(rule.instanceId);state.candidate=identity;state.since=now;trace(rule,state,"observed","subject-dwell",identity);}
    if(now-state.since<rule.dwellMs || (state.lastFired>=0 && now-state.lastFired<rule.cooldownMs)) continue;
    if(state.fired.size()>=256) {trace(rule,state,"blocked","subject-history-capacity",identity);continue;}
    state.fired.insert(identity);state.lastCopy=copy;state.lastFired=now;state.ownedSubject=identity;
    if(mode_=="suggest") {suggest({rule.id,rule.instanceId,"cue",name,secondary,subject->identity});trace(rule,state,"suggested","would-show-prepared-lower-third",identity);continue;}
    decisions.push_back({rule.id,rule.instanceId,mode_=="rehearsal"?"cue":"show",name,secondary,subject->identity});
  }
  return decisions;
}
void GraphicsAutomation::result(const Decision& decision,bool accepted,const std::string& reason,uint64_t revision,double) {
  const auto rule=std::find_if(rules_.begin(),rules_.end(),[&](const auto& r){return r.id==decision.ruleId;});if(rule==rules_.end()) return;
  auto& state=states_[rule->id];
  if(accepted && (decision.action=="show"||decision.action=="update")) {state.owned=true;state.revision=revision;if(decision.action=="show") state.firstDraw=-1;}
  trace(*rule,state,accepted?"accepted":"refused",reason,state.ownedSubject,revision);
}
void GraphicsAutomation::composed(const std::string& instanceId,uint64_t revision,double now) {
  for(const auto& rule:rules_) if(rule.instanceId==instanceId) {auto& state=states_[rule.id];if(state.owned && state.revision==revision && state.firstDraw<0) {state.firstDraw=now;trace(rule,state,"draw-submitted","native-first-draw",state.ownedSubject,revision);}}
}
Json GraphicsAutomation::snapshot() const {
  Json::Array rules,holds;for(const auto& rule:rules_) rules.push_back(rule.document);for(const auto& id:manualHolds_) holds.emplace_back(id);
  Json::Array suggestions;for(const auto& [_,decision]:suggestions_) suggestions.emplace_back(Json::Object{{"ruleId",decision.ruleId},{"instanceId",decision.instanceId},{"name",decision.name},{"secondary",decision.secondary},{"binding",decision.binding?contracts::toJson(*decision.binding):Json{}}});
  return Json::Object{{"suggestions",suggestions},{"droppedHistory",double(droppedTrace_)},{"revision",double(revision_)},{"mode",mode_},{"armed",armed_},{"inhibited",inhibited_},{"manualHold",globalHold_},{"heldInstances",holds},{"configuration",Json::Object{{"rules",rules},{"sourceTexts",sourceTexts_}}},{"trace",trace_}};
}
}
