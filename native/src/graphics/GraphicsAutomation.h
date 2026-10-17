#pragma once
#include "contracts/Lifecycle.h"
#include "rpc/Json.h"
#include <map>
#include <set>
#include <vector>
#include <optional>

namespace corevideo::graphics {
class GraphicsAutomation {
 public:
  struct Subject { contracts::SourceInstanceIdentity identity; std::string name; };
  struct Decision {std::string ruleId,instanceId,action,name,secondary;std::optional<contracts::SourceInstanceIdentity> binding;};
  struct Rule {std::string id,instanceId,sceneId,sourceId;bool enabled=true,suppressScreenShare=true;double dwellMs=1500,durationMs=8000,cooldownMs=0;rpc::Json document;};
  static bool readConfiguration(const rpc::Json&,std::vector<Rule>&,rpc::Json&,std::string&);
  bool configure(const rpc::Json&,uint64_t expectedRevision,std::string& error);
  bool control(const std::string& action,const std::string& mode,const std::string& instanceId,bool sourcesAvailable,std::string& error);
  void manual(const std::string& instanceId);
  void clear();
  void observeEpoch(uint64_t epoch);
  bool observing() const {return mode_!="manual" && !rules_.empty() && (mode_=="suggest"||armed_) && !inhibited_ && !globalHold_;}
  uint64_t traceSequence() const {return traceSequence_+suggestionVersion_;}
  bool owns(const std::string& instanceId) const;
  const std::vector<Rule>& rules() const {return rules_;}
  uint64_t revision() const {return revision_;}
  std::vector<Decision> tick(const std::vector<Subject>&,const std::string& sceneId,bool sharing,double nowMs);
  void result(const Decision&,bool accepted,const std::string& reason,uint64_t revision,double nowMs);
  void composed(const std::string& instanceId,uint64_t revision,double nowMs);
  rpc::Json snapshot() const;
 private:
  struct State {std::string candidate,lastCopy,lastNotice,ownedSubject;double since=0,lastFired=-1,firstDraw=-1;uint64_t revision=0;bool owned=false;std::set<std::string> fired;};
  void suggest(const Decision&);
  void forgetSuggestion(const std::string&);
  void trace(const Rule&,State&,const std::string& state,const std::string& reason,const std::string& subject={},uint64_t revision=0);
  std::vector<Rule> rules_;
  std::map<std::string,State> states_;
  std::map<std::string,Decision> suggestions_;
  std::set<std::string> manualHolds_;
  rpc::Json sourceTexts_=rpc::Json::Object{};
  rpc::Json::Array trace_;
  std::string mode_="manual";
  bool armed_=false,inhibited_=false,globalHold_=false;
  uint64_t revision_=1,sourceEpoch_=0,traceSequence_=0,droppedTrace_=0,suggestionVersion_=0;
};
}
