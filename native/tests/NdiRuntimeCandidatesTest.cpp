// Task 8 (mac-n1): where the macOS core looks for the operator's NDI runtime,
// and what it says when there is none. The runtime is NEVER bundled (owner
// decision OD-1 = No, matching Windows): the core dlopens it from the
// operator's NDI install, so the search order and the missing-runtime sentence
// are the whole user-visible contract.
#include "modules/Interfaces.h"
#include "modules/NdiRuntimeCandidates.h"

#include <gtest/gtest.h>

#include <map>
#include <string>
#include <vector>

namespace {
using namespace corevideo::modules;

// A getenv that answers only from a fixed table: the real environment of the
// test process (which may or may not carry NDI_RUNTIME_DIR*) never leaks in.
struct FakeEnv {
  std::map<std::string, std::string> values;
  const char* operator()(const char* name) const {
    const auto found = values.find(name);
    return found == values.end() ? nullptr : found->second.c_str();
  }
};

#if defined(__APPLE__)
TEST(NdiRuntimeCandidates, AppleOrderAndEnvironment) {
  FakeEnv env{{{"NDI_RUNTIME_DIR_V6", "/opt/x"}}};
  const auto candidates = ndiLibraryCandidates(env);
  ASSERT_FALSE(candidates.empty());
  EXPECT_EQ(candidates.front(), "/opt/x/libndi.dylib");
  EXPECT_EQ(candidates.back(), "libndi.dylib");
  for (const auto& candidate : candidates) {
    EXPECT_FALSE(candidate.empty());
  }
  // Exactly one environment entry: V5 and the unversioned variable are unset.
  int envEntries = 0;
  for (const auto& candidate : candidates) {
    if (candidate.rfind("/opt/", 0) == 0) ++envEntries;
  }
  EXPECT_EQ(envEntries, 1);
}

TEST(NdiRuntimeCandidates, AppleEnvironmentVariablesComeFirstInVersionOrder) {
  FakeEnv env{{{"NDI_RUNTIME_DIR", "/opt/base"},
               {"NDI_RUNTIME_DIR_V5", "/opt/v5/"},
               {"NDI_RUNTIME_DIR_V6", "/opt/v6"}}};
  const auto candidates = ndiLibraryCandidates(env);
  ASSERT_GE(candidates.size(), 4u);
  EXPECT_EQ(candidates[0], "/opt/v6/libndi.dylib");
  EXPECT_EQ(candidates[1], "/opt/v5/libndi.dylib");  // a trailing slash does not double up
  EXPECT_EQ(candidates[2], "/opt/base/libndi.dylib");
  EXPECT_EQ(candidates.back(), "libndi.dylib");
}

TEST(NdiRuntimeCandidates, AppleUnsetOrEmptyVariablesAddNothing) {
  FakeEnv env{{{"NDI_RUNTIME_DIR_V6", ""}}};
  const std::vector<std::string> expected = {
      "/usr/local/lib/libndi.dylib",
      "/Library/NDI SDK for Apple/lib/macOS/libndi.dylib",
      "libndi.dylib",
  };
  EXPECT_EQ(ndiLibraryCandidates(env), expected);
  EXPECT_EQ(ndiLibraryCandidates(FakeEnv{}), expected);
}

TEST(NdiRuntimeCandidates, AppleMissingRuntimeSentenceNamesNdiTools) {
  EXPECT_EQ(ndiRuntimeMissingWarning("missing:libNDI runtime"),
            "NDI output needs the NDI runtime. Install NDI Tools from ndi.video, then restart CoreVideo Pro.");
}
#endif

#if !defined(_WIN32) && !COREVIDEO_STUB && COREVIDEO_ENABLE_DEV_ADAPTERS && COREVIDEO_WITH_NDI_OUTPUT
// Drives the REAL sender (rule 10): an empty candidate set is a machine with
// no NDI install, whatever this test machine actually has.
TEST(NdiRuntimeCandidates, MissingRuntimeSentenceNamesNdiTools) {
  auto sender = createNdiOutputSenderWithCandidatesForTest({});
  ASSERT_NE(sender, nullptr);
  EXPECT_FALSE(sender->runtimeAvailableAtConstruction());
  OutputDestinationSettings settings;
  settings.id = "ndi";
  settings.ndiName = "CoreVideo Pro Program";
  const auto session = sender->sync({"ndi"}, nullptr, 0.0, {settings});
  ASSERT_FALSE(session.senders.empty());
  const auto& row = session.senders.front();
  EXPECT_EQ(row.lastResultCode, "runtime-missing");
#if defined(__APPLE__)
  EXPECT_EQ(row.warning,
            "NDI output needs the NDI runtime. Install NDI Tools from ndi.video, then restart CoreVideo Pro.");
#endif
  // The snapshot appends live counters (" connections=N ...") to the probe
  // detail, so pin the probe's own words as the prefix.
  EXPECT_EQ(row.runtimeDetail.rfind("missing:libNDI runtime", 0), 0u) << row.runtimeDetail;
}
#endif

}  // namespace
