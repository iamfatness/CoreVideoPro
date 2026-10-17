#include "graphics/GraphicsClock.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <ctime>

namespace corevideo::graphics {
using rpc::Json;
bool GraphicsClockConfig::read(const Json& value,GraphicsClockConfig& out) {
  if (!value.isObject() || (value.asObject().size()!=6 && value.asObject().size()!=7)) return false;
  for (const auto* key : {"mode","zone","format","finish","offsetMinutes","durationSeconds"}) if (!value.get(key)) return false;
  GraphicsClockConfig c;
  for (const auto& entry:value.asObject()) {
    const auto& key=entry.first; const auto& v=entry.second;
    if (key=="restoreRemainingSeconds") { if(v.isNull()) continue; if(!v.isNumber() || !std::isfinite(v.asNumber()) || v.asNumber()<0 || v.asNumber()>86400) return false; c.restoreRemainingSeconds=v.asNumber(); }
    else if (key=="offsetMinutes" || key=="durationSeconds") {
      if (!v.isNumber() || !std::isfinite(v.asNumber()) || std::floor(v.asNumber())!=v.asNumber() || v.asNumber() < -840 || v.asNumber()>86400) return false;
      if (key=="offsetMinutes") c.offsetMinutes=int(v.asNumber()); else c.durationSeconds=int(v.asNumber());
    } else {
      if (!v.isString()) return false;
      if (key=="mode") c.mode=v.asString(); else if (key=="zone") c.zone=v.asString();
      else if (key=="format") c.format=v.asString(); else if (key=="finish") c.finish=v.asString(); else return false;
    }
  }
  if ((c.mode!="wall" && c.mode!="countdown") || (c.zone!="local" && c.zone!="utc" && c.zone!="fixed") ||
      (c.format!="hms" && c.format!="hm") || (c.finish!="hold-zero" && c.finish!="hide" && c.finish!="suggest-next") ||
      c.offsetMinutes < -840 || c.offsetMinutes>840 || c.durationSeconds<1 || c.durationSeconds>86400 || (c.restoreRemainingSeconds && *c.restoreRemainingSeconds>c.durationSeconds)) return false;
  out=c; return true;
}
Json GraphicsClockConfig::json() const { return Json::Object{{"mode",mode},{"zone",zone},{"format",format},{"finish",finish},{"offsetMinutes",offsetMinutes},{"durationSeconds",durationSeconds}}; }
void GraphicsClock::control(const std::string& action,double now,const GraphicsClockConfig* replacement) {
  if (replacement) { config=*replacement; remainingSeconds_=config.restoreRemainingSeconds.value_or(config.durationSeconds); config.restoreRemainingSeconds.reset(); running_=false; anchorMs_=now; return; }
  if (config.mode!="countdown") return;
  const auto remaining=std::max(0.,remainingSeconds_-(running_ ? (now-anchorMs_)/1000. : 0.));
  if (action=="reset") { remainingSeconds_=config.durationSeconds; running_=false; anchorMs_=now; }
  else if (action=="pause" && running_) { remainingSeconds_=remaining; running_=false; anchorMs_=now; }
  else if ((action=="start" || action=="resume") && !running_ && remaining>0) { remainingSeconds_=remaining; running_=true; anchorMs_=now; }
}
Json GraphicsClock::sample(double now,int64_t wallSeconds) const {
  char text[32]{}; bool finished=false; double remaining=0;
  if (config.mode=="countdown") {
    remaining=std::max(0.,remainingSeconds_-(running_ ? std::max(0.,now-anchorMs_)/1000. : 0.));
    const int seconds=int(std::ceil(remaining)); finished=seconds==0;
    std::snprintf(text,sizeof(text),"%02d:%02d:%02d",seconds/3600,(seconds/60)%60,seconds%60);
  } else {
    const auto shifted=std::time_t(wallSeconds+(config.zone=="fixed" ? config.offsetMinutes*60 : 0)); std::tm time{};
#ifdef _WIN32
    if (config.zone=="local") localtime_s(&time,&shifted); else gmtime_s(&time,&shifted);
#else
    if (config.zone=="local") localtime_r(&shifted,&time); else gmtime_r(&shifted,&time);
#endif
    std::strftime(text,sizeof(text),config.format=="hm" ? "%H:%M" : "%H:%M:%S",&time);
  }
  return Json::Object{{"text",text},{"state",config.mode=="wall" ? "wall" : finished ? "finished" : running_ ? "running" : "paused"},
      {"remainingSeconds",remaining},{"configuration",config.json()},{"finishAction",finished ? config.finish : ""}};
}
GraphicsClockController::GraphicsClockController(GraphicsClockConfig initial) { worker_=std::thread([this,initial]{run(initial);}); }
GraphicsClockController::~GraphicsClockController() { { std::lock_guard lock(mutex_); stopping_=true; } wake_.notify_one(); if(worker_.joinable()) worker_.join(); }
bool GraphicsClockController::submit(const std::string& action,const GraphicsClockConfig& config) {
  std::unique_lock lock(mutex_,std::try_to_lock); if (!lock.owns_lock() || commands_.size()>=8 || stopping_) return false;
  commands_.push_back({action,config}); wake_.notify_one(); return true;
}
void GraphicsClockController::run(GraphicsClockConfig initial) {
  GraphicsClock clock; clock.control("configure",0,&initial); std::string previous;
  for (;;) {
    std::deque<Command> commands;
    { std::unique_lock lock(mutex_); if (!previous.empty()) wake_.wait_for(lock,std::chrono::milliseconds(100),[&]{return stopping_ || !commands_.empty();});
      if(stopping_) return; commands.swap(commands_); }
    const auto now=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now().time_since_epoch()).count();
    for(const auto& command:commands) clock.control(command.action,now,command.action=="configure" ? &command.config : nullptr);
    auto value=clock.sample(now,std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count());
    const auto signature=value.stringify(); if(signature==previous) continue; previous=signature;
    std::atomic_store(&sample_,std::make_shared<const Json>(std::move(value)));
  }
}
}
