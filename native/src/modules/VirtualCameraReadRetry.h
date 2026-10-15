#pragma once
#include "modules/VirtualCameraDeliveryEvidence.h"
#include <algorithm>

namespace corevideo::modules {
// The caller supplies the original sample deadline. A delayed worker must not
// receive a fresh timeout; waiting never authorizes a third payload copy.
template <typename Read, typename Now, typename Wait>
bool readCameraUntilDeadline(Read read, Now nowUs, Wait wait, bool enabled,
                            unsigned& retries, int64_t deadlineUs, unsigned maxRetries = 8, int64_t recoveryBudgetUs = 2000) {
  retries = 0;
  auto result = read(true);
  // Cold mapping/page faults and the first bounded payload copy can exceed
  // 2 ms. They consume the original sample deadline, not the recovery window.
  // A late first read still gets no time beyond that original deadline.
  const auto retryDeadlineUs = (std::min)(deadlineUs, nowUs() + (std::max)(int64_t(0), recoveryBudgetUs));
  while (enabled && (result == VirtualCameraReadResult::Unchanged ||
                     result == VirtualCameraReadResult::Contended) &&
         retries < maxRetries && nowUs() < retryDeadlineUs) {
    if (!wait(retryDeadlineUs - nowUs())) break;
    if (nowUs() >= retryDeadlineUs) break;
    ++retries;
    result = read(false);
  }
  return result == VirtualCameraReadResult::Fresh;
}

// Retry only transient publication races. Bound both elapsed time and copy
// attempts; an absent/broken publisher must not turn into a busy polling loop.
template <typename Read, typename Now, typename Wait>
bool readCameraWithBoundedRetry(Read read, Now nowUs, Wait wait, bool enabled,
                               unsigned& retries, int64_t budgetUs = 2000, unsigned maxRetries = 3) {
  retries = 0;
  const auto deadline = nowUs() + budgetUs;
  auto result = read(true);
  while (enabled && (result == VirtualCameraReadResult::Unchanged ||
                     result == VirtualCameraReadResult::Contended) &&
         retries < maxRetries && nowUs() < deadline) {
    if (!wait()) break;
    if (nowUs() >= deadline) break;
    ++retries;
    result = read(false);
  }
  return result == VirtualCameraReadResult::Fresh;
}
}
