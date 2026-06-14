#include "core/Protocol.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

namespace {

std::string readRepoFile(const std::string& relativePath) {
  const std::filesystem::path root = COREVIDEO_REPO_ROOT;
  std::ifstream input(root / relativePath);
  if (!input) {
    return {};
  }
  std::ostringstream buffer;
  buffer << input.rdbuf();
  return buffer.str();
}

template <typename Strings>
void expectAllStringsPresent(const std::string& source, const Strings& strings) {
  for (auto value : strings) {
    EXPECT_NE(source.find(std::string(value)), std::string::npos) << "Missing protocol string: " << value;
  }
}

}  // namespace

TEST(ContractParity, MediaCoreCommandTypesMatchTypeScriptProtocol) {
  const std::string source = readRepoFile("src/engine/nativeMediaCoreProtocol.ts");
  ASSERT_FALSE(source.empty());
  expectAllStringsPresent(source, corevideo::core::kNativeMediaCoreCommandTypes);
}

TEST(ContractParity, CapabilityStringsMatchTypeScriptProtocol) {
  const std::string source = readRepoFile("src/engine/nativeMediaCoreProtocol.ts");
  ASSERT_FALSE(source.empty());
  expectAllStringsPresent(source, corevideo::core::kNativeMediaCoreCapabilities);
  expectAllStringsPresent(source, corevideo::core::kRequiredMvpCapabilities);
}

TEST(ContractParity, BridgeEnvelopeTypesMatchTypeScriptProtocol) {
  const std::string source = readRepoFile("src/engine/nativeBridgeProtocol.ts");
  ASSERT_FALSE(source.empty());
  expectAllStringsPresent(source, corevideo::core::kNativeBridgeCommandTypes);
  EXPECT_NE(source.find("id: string"), std::string::npos);
  EXPECT_NE(source.find("ok: true"), std::string::npos);
  EXPECT_NE(source.find("ok: false"), std::string::npos);
}

TEST(ContractParity, ZoomMediaSpineSyncMirrorsTypeScriptProtocolNames) {
  const std::string payloadSource = readRepoFile("src/engine/zoomMediaSpineSync.ts");
  const std::string snapshotSource = readRepoFile("src/engine/zoomMediaSpineNativeSync.ts");
  ASSERT_FALSE(payloadSource.empty());
  ASSERT_FALSE(snapshotSource.empty());
  EXPECT_NE(payloadSource.find("ZoomMediaSpineSyncPayload"), std::string::npos);
  EXPECT_NE(snapshotSource.find("ZoomMediaSpineNativeSnapshot"), std::string::npos);
  expectAllStringsPresent(payloadSource + snapshotSource, corevideo::core::kZoomMediaSpineSyncTypeNames);
}

TEST(ContractParity, ZoomMediaSpineSyncRequestTypeIsMirrored) {
  const std::string desktopProtocol = readRepoFile("desktop/coreProtocol.ts");
  const std::string nodeProtocol = readRepoFile("native-core/src/protocol.ts");
  const std::string milestoneBrief = readRepoFile("docs/agent-briefs/04-track-b-next-milestones.md");
  ASSERT_FALSE(nodeProtocol.empty());
  ASSERT_FALSE(milestoneBrief.empty());

  expectAllStringsPresent(nodeProtocol, corevideo::core::kCoreRequestTypes);
  if (desktopProtocol.find("ZoomMediaSpineSyncPayload") != std::string::npos) {
    expectAllStringsPresent(desktopProtocol, corevideo::core::kCoreRequestTypes);
  } else {
    expectAllStringsPresent(milestoneBrief, corevideo::core::kCoreRequestTypes);
  }
}
