// CoreVideo Edge (not ported): the P2E comms_* command lines, parsed.
//
// Shared by the real engine (engine-comms.cpp), the fake engine and the tests
// so all three read a command the same way. The engine's own json_str/
// json_uint helpers cannot carry a bool, a negative number or "slot":"all",
// so this is a tiny, strict field reader: a field that is present but of the
// wrong type is an ERROR, never a silent default (an "on" that is not a bool
// must not key a channel).
//
// Verbs (docs/PROTOCOL.md, "Engine comms lines"):
//   comms_key          {"slot":0..15|"all","on":bool[,"lease_ms":0..60000]}
//                      lease_ms > 0: the hold expires unless re-sent within it
//                      (a hung control client cannot leave a mic open)
//   comms_release_all  {}  every HELD key and ALL CALL up (latches stay)
//   comms_latch        {"slot":0..15|"all","on":bool}
//   comms_assign       {"slot":0..15,"participant_id":uint,"on":bool}
//   comms_mic          {"device":string}            ("" = system default)
//   comms_sidetone_out {"device":string}            ("" = system default)
//   comms_sidetone     {"on":bool}
//   comms_tone         {"on":bool}
//   comms_gain         {"db":int -20..20}
#pragma once

#include <cstdint>
#include <cstdlib>
#include <string>

namespace edgecomms {

enum class CommsVerb { None, Key, Latch, Assign, Mic, SidetoneOut, Sidetone, Tone, Gain, ReleaseAll };

struct CommsCommand {
  CommsVerb verb = CommsVerb::None;
  int slot = -1;  // -1 = "all" (key/latch only)
  bool on = false;
  uint32_t participant_id = 0;
  std::string device;
  int db = 0;
  int lease_ms = 0;  // comms_key on: 0 = held until released
  std::string error;  // non-empty: refuse, say this
};

constexpr int kCommsSlots = 16;
constexpr int kCommsGainMinDb = -20;
constexpr int kCommsGainMaxDb = 20;

namespace detail {

// Position just past `"key":` (whitespace skipped), or npos.
inline size_t FieldValue(const std::string& line, const std::string& key) {
  const std::string needle = "\"" + key + "\"";
  size_t at = 0;
  while ((at = line.find(needle, at)) != std::string::npos) {
    size_t p = at + needle.size();
    while (p < line.size() && (line[p] == ' ' || line[p] == '\t')) ++p;
    if (p < line.size() && line[p] == ':') {
      ++p;
      while (p < line.size() && (line[p] == ' ' || line[p] == '\t')) ++p;
      return p;
    }
    at = p;
  }
  return std::string::npos;
}

inline void AppendUtf8(std::string& out, uint32_t cp) {
  if (cp < 0x80) {
    out += static_cast<char>(cp);
  } else if (cp < 0x800) {
    out += static_cast<char>(0xC0 | (cp >> 6));
    out += static_cast<char>(0x80 | (cp & 0x3F));
  } else if (cp < 0x10000) {
    out += static_cast<char>(0xE0 | (cp >> 12));
    out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
    out += static_cast<char>(0x80 | (cp & 0x3F));
  } else {
    out += static_cast<char>(0xF0 | (cp >> 18));
    out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
    out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
    out += static_cast<char>(0x80 | (cp & 0x3F));
  }
}

// Reads a JSON string starting at line[p] == '"'. False if malformed.
inline bool ReadString(const std::string& line, size_t p, std::string* out) {
  if (p >= line.size() || line[p] != '"') return false;
  out->clear();
  for (++p; p < line.size(); ++p) {
    const char c = line[p];
    if (c == '"') return true;
    if (c != '\\') {
      *out += c;
      continue;
    }
    if (++p >= line.size()) return false;
    switch (line[p]) {
      case '"': *out += '"'; break;
      case '\\': *out += '\\'; break;
      case '/': *out += '/'; break;
      case 'b': *out += '\b'; break;
      case 'f': *out += '\f'; break;
      case 'n': *out += '\n'; break;
      case 'r': *out += '\r'; break;
      case 't': *out += '\t'; break;
      case 'u': {
        if (p + 4 >= line.size()) return false;
        uint32_t cp = std::strtoul(line.substr(p + 1, 4).c_str(), nullptr, 16);
        p += 4;
        if (cp >= 0xD800 && cp <= 0xDBFF && p + 6 < line.size() && line[p + 1] == '\\' &&
            line[p + 2] == 'u') {
          const uint32_t lo = std::strtoul(line.substr(p + 3, 4).c_str(), nullptr, 16);
          if (lo >= 0xDC00 && lo <= 0xDFFF) {
            cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
            p += 6;
          }
        }
        AppendUtf8(*out, cp);
        break;
      }
      default: return false;
    }
  }
  return false;
}

enum class Field { Missing, Bad, Ok };

inline Field ReadBool(const std::string& line, const std::string& key, bool* out) {
  const size_t p = FieldValue(line, key);
  if (p == std::string::npos) return Field::Missing;
  if (line.compare(p, 4, "true") == 0) { *out = true; return Field::Ok; }
  if (line.compare(p, 5, "false") == 0) { *out = false; return Field::Ok; }
  return Field::Bad;
}

// Integers only: "3.5" and "1e2" are refused rather than truncated.
inline Field ReadInt(const std::string& line, const std::string& key, long long* out) {
  const size_t p = FieldValue(line, key);
  if (p == std::string::npos) return Field::Missing;
  size_t e = p;
  if (e < line.size() && line[e] == '-') ++e;
  const size_t digits = e;
  while (e < line.size() && line[e] >= '0' && line[e] <= '9') ++e;
  if (e == digits || e - digits > 12) return Field::Bad;
  if (e < line.size() && (line[e] == '.' || line[e] == 'e' || line[e] == 'E')) return Field::Bad;
  *out = std::strtoll(line.substr(p, e - p).c_str(), nullptr, 10);
  return Field::Ok;
}

inline Field ReadStr(const std::string& line, const std::string& key, std::string* out) {
  const size_t p = FieldValue(line, key);
  if (p == std::string::npos) return Field::Missing;
  return ReadString(line, p, out) ? Field::Ok : Field::Bad;
}

// The command name, read exactly (never a substring search: the engine's own
// dispatch uses line.find("join") / find("quit"), which a device NAME could
// trip -- comms lines are therefore routed before that dispatch runs).
inline std::string CommandName(const std::string& line) {
  std::string name;
  const size_t p = FieldValue(line, "cmd");
  if (p == std::string::npos || !ReadString(line, p, &name)) return {};
  return name;
}

}  // namespace detail

inline bool IsCommsCommand(const std::string& line) {
  return detail::CommandName(line).rfind("comms_", 0) == 0;
}

inline CommsCommand ParseCommsCommand(const std::string& line) {
  using namespace detail;
  CommsCommand c;
  const std::string name = CommandName(line);
  if (name == "comms_key") c.verb = CommsVerb::Key;
  else if (name == "comms_latch") c.verb = CommsVerb::Latch;
  else if (name == "comms_assign") c.verb = CommsVerb::Assign;
  else if (name == "comms_mic") c.verb = CommsVerb::Mic;
  else if (name == "comms_sidetone_out") c.verb = CommsVerb::SidetoneOut;
  else if (name == "comms_sidetone") c.verb = CommsVerb::Sidetone;
  else if (name == "comms_tone") c.verb = CommsVerb::Tone;
  else if (name == "comms_gain") c.verb = CommsVerb::Gain;
  else if (name == "comms_release_all") c.verb = CommsVerb::ReleaseAll;
  else {
    c.error = "unknown comms command '" + name + "'";
    return c;
  }

  const auto need_on = [&]() {
    if (ReadBool(line, "on", &c.on) != Field::Ok) c.error = name + ": \"on\" must be true or false";
  };
  const auto need_slot = [&](bool allow_all) {
    std::string s;
    if (allow_all && ReadStr(line, "slot", &s) == Field::Ok) {
      if (s == "all") c.slot = -1;
      else c.error = name + ": \"slot\" must be 0..15" + (allow_all ? " or \"all\"" : "");
      return;
    }
    long long v = 0;
    if (ReadInt(line, "slot", &v) != Field::Ok || v < 0 || v >= kCommsSlots) {
      c.error = name + ": \"slot\" must be 0..15" + std::string(allow_all ? " or \"all\"" : "");
      return;
    }
    c.slot = static_cast<int>(v);
  };

  switch (c.verb) {
    case CommsVerb::Key:
    case CommsVerb::Latch: {
      need_slot(true);
      if (c.error.empty()) need_on();
      long long lease = 0;
      const Field lf = ReadInt(line, "lease_ms", &lease);
      if (c.error.empty() && c.verb == CommsVerb::Key && lf != Field::Missing) {
        if (lf != Field::Ok || lease < 0 || lease > 60000) c.error = name + ": \"lease_ms\" must be 0..60000";
        else c.lease_ms = static_cast<int>(lease);
      }
      break;
    }
    case CommsVerb::ReleaseAll:
      break;
    case CommsVerb::Assign: {
      need_slot(false);
      long long id = 0;
      if (c.error.empty() &&
          (ReadInt(line, "participant_id", &id) != Field::Ok || id <= 0 || id > 0xFFFFFFFFLL)) {
        c.error = name + ": \"participant_id\" must be a Zoom user id";
      }
      c.participant_id = static_cast<uint32_t>(id);
      if (c.error.empty()) need_on();
      break;
    }
    case CommsVerb::Mic:
    case CommsVerb::SidetoneOut:
      if (ReadStr(line, "device", &c.device) != Field::Ok) c.error = name + ": \"device\" must be a string";
      break;
    case CommsVerb::Sidetone:
    case CommsVerb::Tone:
      need_on();
      break;
    case CommsVerb::Gain: {
      long long db = 0;
      if (ReadInt(line, "db", &db) != Field::Ok || db < kCommsGainMinDb || db > kCommsGainMaxDb) {
        c.error = name + ": \"db\" must be a whole number from -20 to 20";
      }
      c.db = static_cast<int>(db);
      break;
    }
    case CommsVerb::None: break;
  }
  return c;
}

}  // namespace edgecomms
