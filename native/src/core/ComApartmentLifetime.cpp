#include "core/ComApartmentLifetime.h"

#if defined(_WIN32)
#include <objbase.h>
#endif

namespace corevideo::core {
ComApartmentLifetime::ComApartmentLifetime() {
#if defined(_WIN32)
  initialized_ = SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED));
#endif
}
ComApartmentLifetime::~ComApartmentLifetime() {
#if defined(_WIN32)
  if (initialized_) CoUninitialize();
#endif
}
}  // namespace corevideo::core
