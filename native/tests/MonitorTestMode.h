#pragma once

#include "modules/Interfaces.h"
#include <cstdio>
#include <cstdlib>
#include <stdexcept>
#include <string>

namespace corevideo::test {
// Environment selection is read when the compositor is constructed. Keep the
// guard alive until after its workers are destroyed, including assertion exits.
class ScopedTestEnvironment {
 public:
  ScopedTestEnvironment(const char* name, const char* value) : name_(name) {
    if (const auto* prior = std::getenv(name)) { present_ = true; prior_ = prior; }
    set(value);
  }
  ~ScopedTestEnvironment() { set(present_ ? prior_.c_str() : nullptr); }
  ScopedTestEnvironment(const ScopedTestEnvironment&) = delete;
  ScopedTestEnvironment& operator=(const ScopedTestEnvironment&) = delete;
 private:
  void set(const char* value) {
#ifdef _WIN32
    _putenv_s(name_.c_str(), value ? value : "");
#else
    if (value) setenv(name_.c_str(), value, 1); else unsetenv(name_.c_str());
#endif
  }
  std::string name_, prior_;
  bool present_ = false;
};

class MonitorTestMode {
 public:
  explicit MonitorTestMode(bool isolated)
      : monitor_("COREVIDEO_ISOLATE_MONITORS", isolated ? "1" : "0"),
        cpu_("COREVIDEO_CPU_SOURCE_PREPARATION", "0"), isolated_(isolated) {}
  void verify(const modules::ICompositor& compositor, const char* fixture) const {
    const auto diagnostic = compositor.monitorDiagnostics();
    std::fprintf(stderr, "[gpu-test-mode] fixture=%s requested=%s effective=%s selection=%s cpu_preparation=0\n",
        fixture, isolated_ ? "isolated" : "inline", diagnostic.effectiveMode.c_str(),
        diagnostic.selectionSource.c_str());
    if (compositor.hasIsolatedMonitors() != isolated_ ||
        diagnostic.effectiveMode != (isolated_ ? "isolated" : "inline"))
      throw std::runtime_error("GPU fixture did not select its declared monitor mode");
  }
 private:
  ScopedTestEnvironment monitor_, cpu_;
  bool isolated_;
};
} // namespace corevideo::test
