#include "modules/ZoomCommsRelay.h"

#include <cctype>
#include <cmath>

namespace corevideo::modules {
namespace {

using rpc::Json;

constexpr int kSlots = 16;

std::string camel(const std::string& key) {
  std::string out;
  out.reserve(key.size());
  bool upper = false;
  for (const char c : key) {
    if (c == '_') {
      upper = !out.empty();
      continue;
    }
    out += upper ? static_cast<char>(std::toupper(static_cast<unsigned char>(c))) : c;
    upper = false;
  }
  return out;
}

Json rekey(const Json& v) {
  if (v.isObject()) {
    Json::Object out;
    for (const auto& [key, value] : v.asObject()) out.emplace(camel(key), rekey(value));
    return Json(std::move(out));
  }
  if (v.isArray()) {
    Json::Array out;
    for (const auto& item : v.asArray()) out.push_back(rekey(item));
    return Json(std::move(out));
  }
  return v;
}

std::optional<bool> boolField(const Json& c, const char* key) {
  const auto* v = c.get(key);
  if (!v || !v->isBool()) return std::nullopt;
  return v->asBool();
}

std::optional<long long> intField(const Json& c, const char* key) {
  const auto* v = c.get(key);
  if (!v || !v->isNumber()) return std::nullopt;
  const double d = v->asNumber();
  if (std::trunc(d) != d || std::fabs(d) > 4294967295.0) return std::nullopt;
  return static_cast<long long>(d);
}

std::string engineVerb(const std::string& cmd) {
  std::string out = cmd;
  for (auto& ch : out)
    if (ch == '-') ch = '_';
  return out;  // comms-sidetone-out -> comms_sidetone_out
}

const Json* findRosterEntry(const Json* comms, double participantId) {
  if (!comms) return nullptr;
  const auto* roster = comms->get("roster");
  if (!roster || !roster->isArray()) return nullptr;
  for (const auto& p : roster->asArray())
    if (p.isObject() && p.getNumber("participantId", -1) == participantId) return &p;
  return nullptr;
}

}  // namespace

bool isCommsVerb(const std::string& cmd) {
  return cmd == "comms-key" || cmd == "comms-latch" || cmd == "comms-assign" || cmd == "comms-mic" ||
         cmd == "comms-sidetone-out" || cmd == "comms-sidetone" || cmd == "comms-tone" || cmd == "comms-gain" ||
         cmd == "comms-release-all" || cmd == "comms-private";
}

CommsRelayResult buildCommsEngineLine(const std::string& cmd, const Json& c, bool inMeeting, const Json* lastComms) {
  CommsRelayResult r;
  Json::Object line{{"cmd", engineVerb(cmd)}};
  const bool available = lastComms && lastComms->get("available") && lastComms->get("available")->asBool(false);
  const std::string reason = lastComms ? lastComms->getString("reason") : std::string();

  if (cmd == "comms-key" || cmd == "comms-latch") {
    const auto on = boolField(c, "on");
    const auto all = boolField(c, "all");
    const auto slot = intField(c, "slot");
    const bool wantsAll = all.value_or(false);
    if (!on || (wantsAll && c.get("slot")) || (!wantsAll && (!slot || *slot < 0 || *slot >= kSlots))) {
      r.error = cmd + " needs \"slot\" (0-15) or \"all\":true, and \"on\" (true or false).";
      return r;
    }
    // Releasing is never refused: a key must always be able to come up.
    if (*on) {
      if (!inMeeting) { r.error = "Join a meeting first."; return r; }
      if (!available) {
        r.error = "Comms is not available" + (reason.empty() ? std::string(".") : ": " + reason);
        return r;
      }
    }
    if (wantsAll) line.emplace("slot", "all");
    else line.emplace("slot", static_cast<double>(*slot));
    line.emplace("on", *on);
    // Optional hold lease (comms-key only): the hold drops unless re-sent
    // within leaseMs, so a hung app cannot leave a live mic on talent.
    if (const auto* leaseValue = c.get("leaseMs")) {
      const auto lease = intField(c, "leaseMs");
      if (cmd != "comms-key" || !leaseValue->isNumber() || !lease || *lease < 0 || *lease > 60000) {
        r.error = "\"leaseMs\" (comms-key only) must be a whole number of ms, 0-60000.";
        return r;
      }
      if (*on && *lease > 0) line.emplace("lease_ms", static_cast<double>(*lease));
    }
  } else if (cmd == "comms-release-all") {
    // Every held key and ALL CALL up; never refused (latches stay).
  } else if (cmd == "comms-assign") {
    const auto on = boolField(c, "on");
    const auto slot = intField(c, "slot");
    const auto id = intField(c, "participantId");
    if (!on || !slot || *slot < 0 || *slot >= kSlots || !id || *id <= 0) {
      r.error = "comms-assign needs \"slot\" (0-15), \"participantId\" and \"on\" (true or false).";
      return r;
    }
    if (!inMeeting) { r.error = "Join a meeting first."; return r; }
    if (*on) {
      if (!lastComms) { r.error = "Comms has not reported yet; try again in a moment."; return r; }
      const auto* person = findRosterEntry(lastComms, static_cast<double>(*id));
      if (!person) { r.error = "That participant is not in the meeting."; return r; }
      const auto* tb = person->get("talkback");
      if (tb && tb->isBool() && !tb->asBool()) {
        r.error = person->getString("name", "That participant") +
                  " is on the Zoom web client, which cannot receive talkback. Ask them to rejoin on the Zoom "
                  "desktop or mobile app.";
        return r;
      }
    }
    line.emplace("slot", static_cast<double>(*slot));
    line.emplace("participant_id", static_cast<double>(*id));
    line.emplace("on", *on);
  } else if (cmd == "comms-mic" || cmd == "comms-sidetone-out") {
    const auto* device = c.get("device");
    if (!device || !device->isString()) {
      r.error = cmd + " needs \"device\" (a device name; \"\" for the system default).";
      return r;
    }
    if (!inMeeting) { r.error = "Join a meeting first."; return r; }
    line.emplace("device", device->asString());
  } else if (cmd == "comms-sidetone" || cmd == "comms-tone") {
    const auto on = boolField(c, "on");
    if (!on) { r.error = cmd + " needs \"on\" (true or false)."; return r; }
    if (!inMeeting) { r.error = "Join a meeting first."; return r; }
    line.emplace("on", *on);
  } else if (cmd == "comms-private") {
    // The mic mode (Pro's "Private comms"; a capture-only station is always
    // private and reports private:true whatever is asked). Turning it OFF is
    // never refused; ON needs the meeting the engine runs in.
    const auto on = boolField(c, "on");
    if (!on) { r.error = "comms-private needs \"on\" (true or false)."; return r; }
    if (*on && !inMeeting) { r.error = "Join a meeting first."; return r; }
    line.emplace("on", *on);
  } else if (cmd == "comms-gain") {
    const auto db = intField(c, "db");
    if (!db || *db < -20 || *db > 20) {
      r.error = "comms-gain needs \"db\", a whole number from -20 to 20.";
      return r;
    }
    if (!inMeeting) { r.error = "Join a meeting first."; return r; }
    line.emplace("db", static_cast<double>(*db));
  } else {
    r.error = "Unknown command: " + cmd;
    return r;
  }
  r.engineLine = Json(std::move(line)).stringify();
  return r;
}

std::optional<Json> commsStateForNode(const Json& engineState) {
  if (!engineState.isObject()) return std::nullopt;
  Json::Object out;
  for (const auto& [key, value] : engineState.asObject()) {
    if (key == "cmd") continue;
    out.emplace(camel(key), rekey(value));
  }
  return Json(std::move(out));
}

}  // namespace corevideo::modules
