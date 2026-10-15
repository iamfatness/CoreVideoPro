#pragma once
#include "modules/VirtualCameraDeliveryEvidence.h"

namespace corevideo::modules {
// The caller supplies the original sample deadline. A delayed worker must not
// receive a fresh timeout; waiting never authorizes a third payload copy.
template <typename Read, typename Now, typename Wait>
bool readCameraUntilDeadline(Read read, Now nowUs, Wait wait, bool enabled,
                            unsigned& retries, int64_t deadlineUs, unsigned maxRetries = 8) {
  retries = 0;
  auto result = read(true);
  while (enabled && (result == VirtualCameraReadResult::Unchanged ||
                     result == VirtualCameraReadResult::Contended) &&
         retries < maxRetries && nowUs() < deadlineUs) {
    if (!wait(deadlineUs - nowUs())) break;
    if (nowUs() >= deadlineUs) break;
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
