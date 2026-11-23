#pragma once

// Where a POSIX core looks for the operator's NDI runtime, and what it says
// when there is none (Task 8, mac-n1).
//
// The runtime is NEVER bundled (owner decision OD-1 = No for this phase; the
// Windows build does not bundle it either): bundling libndi is redistribution
// under the NDI SDK licence, and a licence read is still owed before a Mac
// release turns NDI on (OD-1/OD-2). So the core dlopens the operator's own NDI
// install, and this search order plus the missing-runtime sentence are the
// whole user-visible contract.
//
// Pure and header-only so the order is testable on any build, NDI gate or not.
// The Windows candidate list stays in NdiOutputSenderAdapter.cpp, unchanged.

#if !defined(_WIN32)

#include <functional>
#include <string>
#include <vector>

namespace corevideo::modules {

// `getenv` is injected so a test never inherits the machine's real
// NDI_RUNTIME_DIR*; production passes std::getenv.
inline std::vector<std::string> ndiLibraryCandidates(const std::function<const char*(const char*)>& getenv) {
  std::vector<std::string> candidates;
#if defined(__APPLE__)
  const auto addRuntimeDir = [&](const char* variable) {
    const char* dir = getenv ? getenv(variable) : nullptr;
    if (dir == nullptr || dir[0] == '\0') {
      return;  // an unset or empty variable adds nothing (never a bare "/libndi.dylib")
    }
    std::string path(dir);
    if (path.back() != '/') {
      path.push_back('/');
    }
    candidates.push_back(path + "libndi.dylib");
  };
  // Operator overrides first, newest runtime first (the NDI SDK's own names).
  addRuntimeDir("NDI_RUNTIME_DIR_V6");
  addRuntimeDir("NDI_RUNTIME_DIR_V5");
  addRuntimeDir("NDI_RUNTIME_DIR");
  // The install locations. UNVERIFIED on a real install (Task 0 row N-1: this
  // Mac has no NDI install); taken from the plan: NDI Tools' runtime and the
  // NDI SDK for Apple. Re-check with `mdfind -name libndi` on owner hardware.
  candidates.push_back("/usr/local/lib/libndi.dylib");
  candidates.push_back("/Library/NDI SDK for Apple/lib/macOS/libndi.dylib");
  // Last: the bare leaf name (dyld's default search). Note the hardened-runtime
  // core has no allow-dyld-environment-variables entitlement, so DYLD_* is
  // ignored in a signed build; NDI_RUNTIME_DIR* above is the supported override
  // (and a Finder-launched app inherits no shell variables either).
  candidates.push_back("libndi.dylib");
#else
  (void)getenv;
  candidates = {"libndi.so.5", "libndi.so.4", "libndi.so", "libndi.dylib"};
#endif
  return candidates;
}

// The warning a sender publishes when no candidate loaded. On Apple it names
// what to install (a Mac operator has no "libNDI" to look for); elsewhere it
// keeps the original sentence with the probe detail.
inline std::string ndiRuntimeMissingWarning(const std::string& runtimeDetail) {
#if defined(__APPLE__)
  (void)runtimeDetail;
  return "NDI output needs the NDI runtime. Install NDI Tools from ndi.video, then restart CoreVideo Pro.";
#else
  return "NDI sender requires the libNDI runtime on this machine (" + runtimeDetail + ").";
#endif
}

}  // namespace corevideo::modules

#endif  // !_WIN32
