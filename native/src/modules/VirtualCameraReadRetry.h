#pragma once
#include "modules/VirtualCameraDeliveryEvidence.h"

namespace corevideo::modules {
// Frame Server does not inherit the publisher's environment. Recovery must be
// the production default; an explicit reader-process "0" is an A/B diagnostic.
inline bool cameraReadRetryEnabled(const wchar_t* overrideValue) {
  return !(overrideValue && overrideValue[0] == L'0' && overrideValue[1] == L'\0');
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
