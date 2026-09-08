#include "core/BoundedAsyncLog.h"
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
  std::chrono::steady_clock::time_point lastDetailAt{};
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

constexpr auto kProductionWarnInterval = std::chrono::minutes(1);
constexpr auto kVerboseWarnInterval = std::chrono::seconds(1);

}  // namespace

bool LockHoldGuardrail::recordHold(const char* site, long long heldUs, long long budgetUs) {
  const bool overBudget = heldUs > budgetUs;
  bool warn = false;
  bool productionWarning = false;
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
      // Production keeps the first warning and a one-minute aggregate. Health
      // detailed diagnostics restores the one-second cadence for investigations.
      const bool firstWarning = record.stats.warningsLogged == 0;
      const bool productionDue = !firstWarning && now - record.lastWarnAt >= kProductionWarnInterval;
      const bool verboseDue = !firstWarning && ::corevideo::core::nativeVerboseLoggingEnabled() &&
                              now - record.lastDetailAt >= kVerboseWarnInterval;
      if (firstWarning || productionDue || verboseDue) {
        warn = true;
        productionWarning = firstWarning || productionDue;
        ++record.stats.warningsLogged;
        if (firstWarning || productionDue) record.lastWarnAt = now;
        record.lastDetailAt = now;
        suppressed = record.suppressedSinceLastWarn;
        record.suppressedSinceLastWarn = 0;
      } else {
        ++record.suppressedSinceLastWarn;
      }
    }
    snapshot = record.stats;
  }
  if (warn) {
    if (productionWarning) {
      ::corevideo::core::nativeLogf("[lock-guardrail] coreMutex hold %lldus at '%s' exceeds budget %lldus "
                                   "(over-budget %llu of %llu holds, worst %lldus, %llu suppressed)\n",
                                   heldUs, site, budgetUs,
                                   static_cast<unsigned long long>(snapshot.overBudget),
                                   static_cast<unsigned long long>(snapshot.holds), snapshot.worstHeldUs,
                                   static_cast<unsigned long long>(suppressed));
    } else {
      ::corevideo::core::nativeVerboseLogf("[lock-guardrail] coreMutex hold %lldus at '%s' exceeds budget %lldus "
                                          "(over-budget %llu of %llu holds, worst %lldus, %llu suppressed)\n",
                                          heldUs, site, budgetUs,
                                          static_cast<unsigned long long>(snapshot.overBudget),
                                          static_cast<unsigned long long>(snapshot.holds), snapshot.worstHeldUs,
                                          static_cast<unsigned long long>(suppressed));
    }
    if (strictModeEnabled()) {
      ::corevideo::core::nativeLogf("[lock-guardrail] STRICT mode enabled — aborting on over-budget hold.\n");
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
