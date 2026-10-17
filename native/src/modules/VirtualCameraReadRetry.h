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
    // A scheduler wake can cross the deadline while the publication becomes
    // ready. Always inspect once after the bounded wait: abandoning that ready
    // frame manufactured a hold followed by a skipped identity. The deadline
    // still forbids another wait; the shared copy budget bounds this final read.
    ++retries;
    result = read(false);
  }
  return result == VirtualCameraReadResult::Fresh;
}
}
