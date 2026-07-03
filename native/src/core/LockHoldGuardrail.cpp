#include "core/LockHoldGuardrail.h"

#include <cstdio>
#include <cstdlib>
#include <map>
#include <mutex>
#include <string>

namespace corevideo::core {
namespace {

struct SiteRecord {
  LockHoldGuardrail::SiteStats stats;
  std::chrono::steady_clock::time_point lastWarnAt{};
  std::uint64_t suppressedSinceLastWarn = 0;
};

// Function-local statics so the registry is safely initialized on first use
// from any thread (render / command / audio worker / engine sender).
std::mutex& registryMutex() {
  static std::mutex mutex;
  return mutex;
}

std::map<std::string, SiteRecord>& registry() {
  static std::map<std::string, SiteRecord> sites;
  return sites;
}

bool strictModeEnabled() {
  static const bool strict = [] {
    const char* value = std::getenv("COREVIDEO_LOCK_GUARDRAIL_STRICT");
    return value != nullptr && value[0] == '1';
  }();
  return strict;
}

constexpr auto kWarnInterval = std::chrono::seconds(1);

}  // namespace

bool LockHoldGuardrail::recordHold(const char* site, long long heldUs, long long budgetUs) {
  const bool overBudget = heldUs > budgetUs;
  bool warn = false;
  std::uint64_t suppressed = 0;
  SiteStats snapshot;
  {
    std::lock_guard<std::mutex> lock(registryMutex());
    SiteRecord& record = registry()[site];
    ++record.stats.holds;
    if (heldUs > record.stats.worstHeldUs) {
      record.stats.worstHeldUs = heldUs;
    }
    if (overBudget) {
      ++record.stats.overBudget;
      const auto now = std::chrono::steady_clock::now();
      // Rate cap: first over-budget hold per site logs immediately, then at
      // most one warning per second per site (carrying the suppressed count).
      if (record.stats.warningsLogged == 0 || now - record.lastWarnAt >= kWarnInterval) {
        warn = true;
        ++record.stats.warningsLogged;
        record.lastWarnAt = now;
        suppressed = record.suppressedSinceLastWarn;
        record.suppressedSinceLastWarn = 0;
      } else {
        ++record.suppressedSinceLastWarn;
      }
    }
    snapshot = record.stats;
  }
  if (warn) {
    std::fprintf(stderr,
                 "[lock-guardrail] coreMutex hold %lldus at '%s' exceeds budget %lldus "
                 "(over-budget %llu of %llu holds, worst %lldus, %llu suppressed)\n",
                 heldUs, site, budgetUs,
                 static_cast<unsigned long long>(snapshot.overBudget),
                 static_cast<unsigned long long>(snapshot.holds), snapshot.worstHeldUs,
                 static_cast<unsigned long long>(suppressed));
    if (strictModeEnabled()) {
      std::fprintf(stderr, "[lock-guardrail] STRICT mode enabled — aborting on over-budget hold.\n");
      std::abort();
    }
  }
  return overBudget;
}

LockHoldGuardrail::SiteStats LockHoldGuardrail::statsForSite(const char* site) {
  std::lock_guard<std::mutex> lock(registryMutex());
  const auto found = registry().find(site);
  return found == registry().end() ? SiteStats{} : found->second.stats;
}

void LockHoldGuardrail::resetForTest() {
  std::lock_guard<std::mutex> lock(registryMutex());
  registry().clear();
}

}  // namespace corevideo::core
