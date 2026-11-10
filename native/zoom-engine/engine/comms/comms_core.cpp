#include "comms_core.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <ctime>

#include "audio_defs.h"
#include "reach.h"
#include "room_plan.h"

namespace edgecomms {
namespace {

constexpr size_t kLogLines = 50;

std::string JsonEscape(const std::string& s) {
  std::string out;
  out.reserve(s.size() + 8);
  for (const char ch : s) {
    const unsigned char c = static_cast<unsigned char>(ch);
    switch (c) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default:
        if (c < 0x20) {
          char buf[8];
          std::snprintf(buf, sizeof(buf), "\\u%04x", c);
          out += buf;
        } else {
          out += ch;
        }
    }
  }
  return out;
}

std::string Quoted(const std::string& s) { return "\"" + JsonEscape(s) + "\""; }
const char* Bool(bool b) { return b ? "true" : "false"; }

std::string StringArray(const std::vector<std::string>& v) {
  std::string out = "[";
  for (size_t i = 0; i < v.size(); ++i) {
    if (i) out += ",";
    out += Quoted(v[i]);
  }
  return out + "]";
}

std::string ChannelName(int slot) { return "CH " + std::to_string(slot + 1); }

}  // namespace

// ---------------------------------------------------------------------------
// CommsBankSink
// ---------------------------------------------------------------------------

CommsBankSink::CommsBankSink() : voice_gate_(-50.0, 800, zc::kSampleRate) {}

void CommsBankSink::Attach(zc::TalkbackChannels* bank) {
  std::lock_guard<std::mutex> lock(m_);
  bank_ = bank;
}

bool CommsBankSink::CanSend() {
  std::lock_guard<std::mutex> lock(m_);
  return bank_ != nullptr && bank_->channels_ready() > 0;
}

bool CommsBankSink::Send(const int16_t* pcm, int samples) {
  // The peak of what actually leaves for Zoom, post-envelope: the number that
  // separates "transmitting your voice" from "keyed but shipping silence".
  int peak = 0;
  for (int i = 0; i < samples; ++i) {
    const int a = pcm[i] < 0 ? -pcm[i] : pcm[i];
    if (a > peak) peak = a;
  }
  tx_peak_.store(peak);
  voice_active_.store(voice_gate_.Update(pcm, samples));
  std::lock_guard<std::mutex> lock(m_);
  if (bank_ == nullptr) return true;
  // "Nothing keyed" is a normal quiet state, not a send failure.
  if ((bank_->key_mask() & bank_->ready_mask()) == 0) return true;
  return bank_->SendToKeyed(pcm, samples) > 0;
}

// ---------------------------------------------------------------------------
// CommsCore
// ---------------------------------------------------------------------------

CommsCore::CommsCore(CommsAudioPort* audio, CommsBankSink* sink) : audio_(audio), sink_(sink) {}

void CommsCore::Log(const std::string& line) {
  if (!log_.empty()) {
    // Same event twice in a row is one line (ZComms log_op).
    const std::string& last = log_.back();
    if (last.size() > 9 && last.compare(9, std::string::npos, line) == 0) return;
  }
  const std::time_t t = std::time(nullptr);
  std::tm tm{};
#if defined(_WIN32)
  localtime_s(&tm, &t);
#else
  localtime_r(&t, &tm);
#endif
  char stamp[16];
  std::strftime(stamp, sizeof(stamp), "%H:%M:%S ", &tm);
  log_.push_back(stamp + line);
  while (log_.size() > kLogLines) log_.pop_front();
}

void CommsCore::AttachBank(zc::TalkbackChannels* bank) {
  bank_ = bank;
  if (sink_ != nullptr) sink_->Attach(bank);
  next_create_ms_ = 0;
  create_rounds_ = 0;
  create_error_.clear();
  announced_up_ = false;
}

void CommsCore::DetachBank() {
  if (sink_ != nullptr) sink_->Attach(nullptr);  // waits out a Send in flight
  if (audio_ != nullptr) {
    audio_->SetTalk(false);
    audio_->SetRunning(false);
  }
  bank_ = nullptr;
  intent_.clear();
  auto_assigned_.clear();
  invite_backoff_.clear();
  heal_rr_ = 0;
  next_heal_ms_ = next_house_ms_ = next_create_ms_ = 0;
  create_rounds_ = 0;
  create_error_.clear();
  announced_up_ = false;
  latch_mask_ = hold_mask_ = 0;
  hold_all_ = false;
  for (auto& t : hold_until_ms_) t = 0;
  applied_keys_ = prev_keys_ = 0;
  last_ready_ = 0;
  last_ready_change_ms_ = 0;
  duck_ = zc::DuckPlanner{};
  prev_sent_mask_ = 0;
  keyed_silent_since_ = 0;
}

std::string CommsCore::NameOf(unsigned int uid) const {
  for (const zc::RosterMember& m : in_.roster)
    if (m.user_id == uid) return m.name;
  return "user " + std::to_string(uid);
}

void CommsCore::Tick(const CommsInputs& in) {
  const bool was_available = available_;
  const std::string was_reason = reason_;
  in_ = in;
  if (!in_.in_meeting) in_.roster.clear();  // nobody is "here" outside a meeting

  // Availability, most fundamental cause first. Video is never touched here.
  eligible_ = false;
  if (!in.in_meeting) {
    reason_ = "Not in a meeting.";
  } else if (!in.controller) {
    reason_ = "Talkback is unavailable: the Zoom SDK returned no talkback controller.";
  } else if (!in.supported) {
    reason_ = "This meeting does not support Zoom talkback.";
  } else if (!in.host_or_cohost) {
    reason_ = "Make this account host or co-host to bring talkback up.";
  } else if (bank_ == nullptr) {
    reason_ = "Talkback is starting.";
  } else {
    eligible_ = true;
    reason_.clear();
  }

  if (eligible_) {
    const int ready = bank_->channels_ready();
    // The whole bank in ONE call (CreateChannel is rate limited). Re-asked
    // only while nothing at all has been granted; a partial grant is a
    // working intercom, so it is never re-requested.
    if (ready == 0 && in.now_ms >= next_create_ms_) {
      std::string err;
      ++create_rounds_;
      if (!bank_->CreateChannels(kSlots, &err)) {
        create_error_ = err;
        Log("talkback channels refused: " + err);
      } else if (create_rounds_ == 1) {
        Log("requesting " + std::to_string(kSlots) + " talkback channels");
      }
      next_create_ms_ = in.now_ms + 10000;
    }
    if (ready == 0 && create_error_.empty() && !bank_->last_error().empty() && create_rounds_ > 0)
      create_error_ = bank_->last_error();  // the async refusal, e.g. NO_PERMISSION
    if (ready > 0) {
      create_error_.clear();
      if (!announced_up_) {
        announced_up_ = true;
        Log("station up -- " + std::to_string(ready) + " channel(s)");
      }
    }
    if (ready == 0 && !create_error_.empty()) {
      eligible_ = true;  // still retrying in the background
      available_ = false;
      reason_ = "Zoom refused talkback: " + create_error_;
    } else if (!in.mic_blocked.empty()) {
      // Channels may be fine, but nothing keyed would be heard: say so
      // instead of offering keys that deliver silence.
      available_ = false;
      reason_ = in.mic_blocked;
    } else {
      available_ = true;
    }
  } else {
    available_ = false;
  }
  if (available_ != was_available || (!available_ && reason_ != was_reason)) {
    if (available_) Log("talkback available");
    else if (in.in_meeting) Log("talkback unavailable -- " + reason_);
  }

  if (audio_ != nullptr) {
    audio_->SetRunning(eligible_ && bank_ != nullptr && bank_->channels_ready() > 0);
    audio_status_ = audio_->Status();
    if (!audio_status_.error.empty() && audio_status_.error != last_audio_error_)
      Log("microphone: " + audio_status_.error);
    last_audio_error_ = audio_status_.error;
  }

  const int ready_now = bank_ != nullptr ? bank_->channels_ready() : 0;
  if (ready_now != last_ready_) {
    last_ready_ = ready_now;
    last_ready_change_ms_ = in.now_ms;
  }
  const bool bank_settled = ready_now >= kSlots || (ready_now > 0 && in.now_ms - last_ready_change_ms_ >= kSettleMs);
  if (eligible_ && ready_now > 0 && bank_settled) {
    if (in.now_ms >= next_house_ms_) {
      next_house_ms_ = in.now_ms + 2000;
      Housekeeping();
    }
    if (in.now_ms >= next_heal_ms_) {
      next_heal_ms_ = in.now_ms + 600;
      Heal();
    }
  }
  // Expired hold leases: a control client that stopped re-sending (hung,
  // crashed without closing its pipe) must not leave a live mic on talent.
  for (int s = 0; s <= kSlots; ++s) {
    const bool held = s == kSlots ? hold_all_ : ((hold_mask_ >> s) & 1u) != 0;
    if (!held || hold_until_ms_[s] == 0 || in.now_ms < hold_until_ms_[s]) continue;
    hold_until_ms_[s] = 0;
    if (s == kSlots) hold_all_ = false;
    else hold_mask_ &= ~(1u << s);
    Log((s == kSlots ? std::string("ALL CALL") : ChannelName(s)) + " released: its hold was not refreshed");
  }
  ApplyKeys();
  Duck();
  NoticeSends();
}

void CommsCore::ReleaseHolds() {
  if (hold_mask_ == 0 && !hold_all_) return;
  hold_mask_ = 0;
  hold_all_ = false;
  for (auto& t : hold_until_ms_) t = 0;
  Log("every held key released");
  ApplyKeys();
}

void CommsCore::Housekeeping() {
  // A known-ineligible participant (the web client) loses assignment intent.
  zc::PruneIneligibleTalkbackIntent(in_.roster, station_, intent_, auto_assigned_);

  // Prune intent for people no longer here: user ids are meeting-scoped and
  // recycled, so a stale id must not keep a channel occupied. (ZComms skips
  // this while breakouts run; this slice is not breakout-aware, and the
  // engine's roster is the station's room.)
  std::set<unsigned int> present;
  for (const zc::RosterMember& m : in_.roster) present.insert(m.user_id);
  for (auto it = intent_.begin(); it != intent_.end();)
    it = present.count(it->second) ? std::next(it) : intent_.erase(it);
  for (auto it = invite_backoff_.begin(); it != invite_backoff_.end();)
    it = present.count(it->first.second) ? std::next(it) : invite_backoff_.erase(it);
  for (auto it = auto_assigned_.begin(); it != auto_assigned_.end();)
    it = present.count(*it) ? std::next(it) : auto_assigned_.erase(it);

  // Default policy: a new eligible participant gets their OWN channel; past
  // 16 people the least-loaded channel takes the spillover.
  const std::vector<zc::ChannelState> snap = bank_->Snapshot();
  const int nslots = static_cast<int>(snap.size());
  const auto slot_load = [&](int s) {
    std::set<unsigned int> ids =
        zc::EligibleChannelMembers(snap[static_cast<size_t>(s)].members, in_.roster, station_);
    for (const auto& iv : intent_)
      if (iv.first == s) ids.insert(iv.second);
    return static_cast<int>(ids.size());
  };
  for (const zc::RosterMember& m : in_.roster) {
    if (!zc::IsTalkbackEligible(m, station_)) continue;
    if (auto_assigned_.count(m.user_id)) continue;
    int pick = -1, best = 1 << 30;
    for (int s = 0; s < nslots; ++s) {
      if (!snap[static_cast<size_t>(s)].ready) continue;
      const int load = slot_load(s);
      if (load == 0) { pick = s; break; }
      if (load < best) { best = load; pick = s; }
    }
    // Edge edit: remembered as auto-assigned only once a channel took them
    // (ZComms marked first, which strands a person seen before any channel).
    if (pick >= 0) {
      auto_assigned_.insert(m.user_id);
      intent_.insert({pick, m.user_id});
      Log(m.name + " -> " + ChannelName(pick));
    }
  }
}

void CommsCore::Heal() {
  // ONE call per pass, rotated across channels: Zoom's limiter is per CALL.
  const std::vector<zc::ChannelState> snap = bank_->Snapshot();
  const int heal_n = static_cast<int>(snap.size());
  const int64_t now = in_.now_ms;
  for (int k = 0; k < heal_n; ++k) {
    const int s = (heal_rr_ + k) % heal_n;
    std::vector<unsigned int> missing;
    std::string missing_names;
    unsigned int to_remove = 0;
    bool have_remove = false;
    for (const zc::RosterMember& m : in_.roster) {
      const bool want = zc::IsTalkbackEligible(m, station_) && intent_.count({s, m.user_id}) != 0;
      const bool have = snap[static_cast<size_t>(s)].members.count(m.user_id) != 0;
      if (want && have) invite_backoff_.erase({s, m.user_id});
      if (want && !have && m.supports_talkback) {
        const auto b = invite_backoff_.find({s, m.user_id});
        if (b == invite_backoff_.end() || now >= b->second.second) {
          missing.push_back(m.user_id);
          if (!missing_names.empty()) missing_names += ", ";
          missing_names += m.name;
        }
      } else if (!want && have && !have_remove) {
        const auto rb = invite_backoff_.find({s, m.user_id});
        if (rb == invite_backoff_.end() || now >= rb->second.second) {
          to_remove = m.user_id;
          have_remove = true;
        }
      }
    }
    if (!missing.empty()) {
      std::string err;
      const bool ok = bank_->InviteMany(s, missing, &err);
      if (!ok) Log(ChannelName(s) + " invite (" + missing_names + ") failed: " + err);
      for (const unsigned int uid : missing) {
        auto& b = invite_backoff_[{s, uid}];
        b.first = ok ? 0 : b.first + 1;
        // After a successful Execute the join confirmation is async: 10 s of
        // patience before re-asking. A refusal doubles from 5 s to 60 s.
        const int64_t wait_ms = ok ? 10000 : std::min<int64_t>(60000, int64_t{5000} << std::min(b.first, 4));
        b.second = now + wait_ms;
      }
      heal_rr_ = s + 1;
      return;
    }
    if (have_remove) {
      std::string err;
      if (!bank_->Remove(s, to_remove, &err)) {
        Log(ChannelName(s) + " remove failed: " + err);
        auto& b = invite_backoff_[{s, to_remove}];
        b.first += 1;
        b.second = now + 30000;
      }
      heal_rr_ = s + 1;
      return;
    }
  }
}

void CommsCore::ApplyKeys() {
  // Keys only while talkback is up: losing host/co-host or the meeting drops
  // every key and latch-driven send at once.
  uint32_t keys = 0;
  if (available_ && bank_ != nullptr)
    keys = (latch_mask_ | hold_mask_ | (hold_all_ ? kAllSlots : 0u)) & kAllSlots;
  if (bank_ != nullptr) {
    for (int s = 0; s < kSlots; ++s) bank_->SetKey(s, ((keys >> s) & 1u) != 0);
  }
  // A keyed line whose person has not landed yet is audio to nobody: say it,
  // once per key-down, only for channels somebody is MEANT to be on.
  const uint32_t newly = keys & ~prev_keys_;
  if (newly != 0 && bank_ != nullptr) {
    const std::vector<zc::ChannelState> ks = bank_->Snapshot();
    for (int s = 0; s < kSlots && s < static_cast<int>(ks.size()); ++s) {
      if (((newly >> s) & 1u) == 0) continue;
      if (!ks[static_cast<size_t>(s)].members.empty()) continue;
      bool meant = false;
      for (const auto& iv : intent_)
        if (iv.first == s) { meant = true; break; }
      if (meant) Log(ChannelName(s) + " keyed -- nobody is in it yet (invite in flight)");
    }
  }
  if (keys != 0 && !in_.mic_open && prev_keys_ == 0)
    Log("keyed while the meeting mic is closed -- talkback is accepted but SILENT until it opens");
  prev_keys_ = keys;
  applied_keys_ = keys;
  // The envelope opens when any channel is keyed; routing happens in the sink.
  if (audio_ != nullptr) audio_->SetTalk(keys != 0);
}

void CommsCore::Duck() {
  // Volume calls need the role too: without it every pass would be a refused
  // SDK call (and a refusal backs the planner off anyway).
  if (bank_ == nullptr || !eligible_) return;
  const uint32_t activity = (sink_ != nullptr && sink_->voice_active()) ? applied_keys_ : 0u;
  zc::VolumeAction va;
  if (duck_.Next(bank_->ready_mask(), activity, in_.now_ms, &va)) {
    if (bank_->SetChannelVolume(va.slot, va.volume)) duck_.Confirm(va);
    else duck_.Fail(in_.now_ms);
  }
}

void CommsCore::NoticeSends() {
  if (bank_ == nullptr) return;
  const uint32_t now_sent = bank_->sent_mask();
  const uint32_t fresh = now_sent & ~prev_sent_mask_;
  for (int s = 0; s < kSlots; ++s)
    if ((fresh >> s) & 1u) Log("audio flowing into " + ChannelName(s));
  prev_sent_mask_ = now_sent;
  // Keyed but shipping silence while the mic is live at capture: the half of
  // a no-audio hunt that send counters alone cannot show.
  if (applied_keys_ != 0 && sink_ != nullptr && audio_status_.peak_dbfs &&
      *audio_status_.peak_dbfs > -30.0 && sink_->tx_peak() < 100 && !audio_status_.tone) {
    if (keyed_silent_since_ == 0) {
      keyed_silent_since_ = in_.now_ms;
    } else if (in_.now_ms - keyed_silent_since_ > 1500) {
      Log("keyed but transmitting SILENCE (mic live at capture)");
      keyed_silent_since_ = in_.now_ms;
    }
  } else {
    keyed_silent_since_ = 0;
  }
}

std::string CommsCore::Apply(const CommsCommand& c, int64_t now_ms) {
  if (!c.error.empty()) return c.error;
  const auto unavailable = [&]() { return "Comms is not available: " + reason_; };
  switch (c.verb) {
    case CommsVerb::Key:
      if (c.on && !available_) return unavailable();
      if (c.slot < 0) hold_all_ = c.on;
      else if (c.on) hold_mask_ |= 1u << c.slot;
      else hold_mask_ &= ~(1u << c.slot);
      hold_until_ms_[c.slot < 0 ? kSlots : c.slot] = (c.on && c.lease_ms > 0) ? now_ms + c.lease_ms : 0;
      ApplyKeys();  // PTT must not wait for the next tick
      return {};
    case CommsVerb::Latch:
      if (c.on && !available_) return unavailable();
      if (c.slot < 0) latch_mask_ = c.on ? kAllSlots : 0u;
      else if (c.on) latch_mask_ |= 1u << c.slot;
      else latch_mask_ &= ~(1u << c.slot);
      ApplyKeys();
      return {};
    case CommsVerb::Assign: {
      if (bank_ == nullptr || !eligible_) return unavailable();
      const std::string who = NameOf(c.participant_id);
      if (!c.on) {
        intent_.erase({c.slot, c.participant_id});
        Log(who + " off " + ChannelName(c.slot));
        return {};
      }
      const zc::RosterMember* member = nullptr;
      for (const zc::RosterMember& m : in_.roster)
        if (m.user_id == c.participant_id) member = &m;
      if (member == nullptr) return "Participant " + std::to_string(c.participant_id) + " is not in the meeting.";
      if (member->name == station_) return "That is this station; it cannot talk to itself.";
      if (!member->supports_talkback)
        return member->name +
               " cannot receive talkback: they are on the Zoom web client. Ask them to rejoin on the Zoom desktop or mobile app.";
      {
        const std::vector<zc::ChannelState> snap = bank_->Snapshot();
        if (c.slot >= static_cast<int>(snap.size()) || !snap[static_cast<size_t>(c.slot)].ready)
          return ChannelName(c.slot) + " is not ready yet.";
      }
      if (!zc::CanAssignTalkback(in_.roster, station_, c.participant_id, c.slot, kSlots))
        return member->name + " cannot be assigned to " + ChannelName(c.slot) + ".";
      intent_.insert({c.slot, c.participant_id});
      auto_assigned_.insert(c.participant_id);  // the operator placed them; auto leaves them be
      next_heal_ms_ = 0;                         // heal on the next tick
      Log(who + " -> " + ChannelName(c.slot));
      return {};
    }
    case CommsVerb::Mic:
      if (audio_) audio_->SetMic(c.device);
      Log("microphone -> " + (c.device.empty() ? std::string("system default") : c.device));
      return {};
    case CommsVerb::SidetoneOut:
      if (audio_) audio_->SetSidetoneOut(c.device);
      Log("sidetone output -> " + (c.device.empty() ? std::string("system default") : c.device));
      return {};
    case CommsVerb::Sidetone:
      if (audio_) audio_->SetSidetone(c.on);
      return {};
    case CommsVerb::Tone:
      if (audio_) audio_->SetTone(c.on);
      Log(c.on ? "test tone ON -- replaces the mic in the live chain" : "test tone off");
      return {};
    case CommsVerb::Gain:
      if (audio_) audio_->SetGain(c.db);
      return {};
    case CommsVerb::ReleaseAll:
      ReleaseHolds();
      return {};
    case CommsVerb::None: break;
  }
  return "unknown comms command";
}

std::string CommsCore::StateJson(bool with_peak) const {
  const std::vector<zc::ChannelState> snap = bank_ != nullptr ? bank_->Snapshot() : std::vector<zc::ChannelState>{};
  std::map<unsigned int, std::string> names;  // eligible people only
  for (const zc::RosterMember& m : in_.roster)
    if (zc::IsTalkbackEligible(m, station_)) names[m.user_id] = m.name;

  // Members a channel holds or is meant to hold, eligible only.
  const auto population = [&](int s) {
    std::set<unsigned int> ids;
    if (s < static_cast<int>(snap.size())) ids = snap[static_cast<size_t>(s)].members;
    for (const auto& iv : intent_)
      if (iv.first == s) ids.insert(iv.second);
    return zc::EligibleChannelMembers(std::move(ids), in_.roster, station_);
  };

  bool any_ready = false, any_hearing = false;
  std::string ch = "[";
  const zc::BreakoutState no_breakouts;  // this slice: everyone is in the station's room
  for (int s = 0; s < kSlots; ++s) {
    const bool have = s < static_cast<int>(snap.size());
    const bool ready = have && snap[static_cast<size_t>(s)].ready;
    const int listeners =
        have ? static_cast<int>(zc::EligibleChannelMembers(snap[static_cast<size_t>(s)].members, in_.roster, station_).size())
             : 0;
    const bool keyed = ((applied_keys_ >> s) & 1u) != 0;
    const bool latched = ((latch_mask_ >> s) & 1u) != 0;
    any_ready = any_ready || ready;
    any_hearing = any_hearing || (keyed && listeners > 0);
    const std::set<unsigned int> pop = population(s);
    std::vector<std::string> member_names;
    for (const unsigned int id : pop) {
      const auto it = names.find(id);
      if (it != names.end()) member_names.push_back(it->second);
    }
    const zc::ChannelReach reach = zc::ReachFor(no_breakouts, member_names);
    // A channel whose whole population is one person is that person's line.
    std::string label = "null";
    if (pop.size() == 1) {
      const auto it = names.find(*pop.begin());
      if (it != names.end()) label = Quoted(it->second);
    }
    if (s) ch += ",";
    ch += "{\"slot\":" + std::to_string(s) + ",\"label\":" + label + ",\"listeners\":" + std::to_string(listeners) +
          ",\"ready\":" + Bool(ready) + ",\"keyed\":" + Bool(keyed) + ",\"latched\":" + Bool(latched) +
          ",\"room\":null,\"reach_ok\":" + std::to_string(reach.reachable.size()) +
          ",\"reach_dark\":" + std::to_string(reach.elsewhere.size()) + "}";
  }
  ch += "]";

  std::string roster = "[";
  bool first = true;
  for (const zc::RosterMember& m : in_.roster) {
    if (m.name == station_) continue;  // the station's own ghost id in a room move
    if (!first) roster += ",";
    first = false;
    std::string chans = "[";
    bool cf = true;
    for (int s = 0; s < kSlots; ++s) {
      const bool on = (s < static_cast<int>(snap.size()) && snap[static_cast<size_t>(s)].members.count(m.user_id)) ||
                      intent_.count({s, m.user_id});
      if (!on) continue;
      if (!cf) chans += ",";
      cf = false;
      chans += std::to_string(s);
    }
    chans += "]";
    roster += "{\"participant_id\":" + std::to_string(m.user_id) + ",\"name\":" + Quoted(m.name) +
              ",\"talkback\":" + Bool(m.supports_talkback) + ",\"room\":null,\"channels\":" + chans + "}";
  }
  roster += "]";

  const CommsAudioStatus& a = audio_status_;
  std::string peak = "null";
  if (with_peak && a.peak_dbfs) peak = std::to_string(static_cast<int>(std::lround(*a.peak_dbfs)));
  std::string mic = "{\"device\":" + Quoted(a.device) + ",\"devices\":" + StringArray(a.devices) +
                    ",\"peak_dbfs\":" + peak + ",\"gain_db\":" + std::to_string(a.gain_db) +
                    ",\"sidetone\":" + Bool(a.sidetone) + ",\"sidetone_device\":" + Quoted(a.sidetone_device) +
                    ",\"sidetone_devices\":" + StringArray(a.sidetone_devices) + ",\"tone\":" + Bool(a.tone) + "}";

  const uint64_t channel_sends = bank_ != nullptr ? bank_->channel_sends() : 0;
  const uint64_t send_errors = bank_ != nullptr ? bank_->send_failures() : 0;
  std::string stats = "{\"frames_sent\":" + std::to_string(a.frames_sent) +
                      ",\"channel_sends\":" + std::to_string(channel_sends) +
                      ",\"underruns\":" + std::to_string(a.underruns) +
                      ",\"send_errors\":" + std::to_string(send_errors) + "}";

  std::string log = "[";
  for (size_t i = 0; i < log_.size(); ++i) {
    if (i) log += ",";
    log += Quoted(log_[i]);
  }
  log += "]";

  const bool tx = any_hearing && applied_keys_ != 0;
  std::string out = "{\"cmd\":\"comms_state\",\"available\":" + std::string(Bool(available_)) +
                    ",\"reason\":" + (available_ ? std::string("null") : Quoted(reason_)) +
                    ",\"lamps\":{\"link\":" + Bool(in_.in_meeting) + ",\"meeting_mic\":" + Bool(in_.in_meeting && in_.mic_open) +
                    ",\"channel\":" + Bool(any_ready) + ",\"tx\":" + Bool(tx) + "},\"room\":null,\"channels\":" + ch +
                    ",\"roster\":" + roster + ",\"mic\":" + mic + ",\"stats\":" + stats + ",\"log\":" + log + "}";
  return out;
}

bool CommsStateEmitter::Next(const CommsCore& core, int64_t now_ms, std::string* out) {
  std::string full = core.StateJson(true);
  if (full == last_full_) return false;
  std::string shape = core.StateJson(false);
  if (shape == last_shape_ && now_ms - last_emit_ms_ < kMeterMs) return false;  // meter only: wait
  last_shape_ = std::move(shape);
  last_full_ = full;
  last_emit_ms_ = now_ms;
  *out = std::move(full);
  return true;
}

}  // namespace edgecomms
