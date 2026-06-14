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

TEST(ContractParity, ZoomMeetingSdkAdapterGateMatchesReadinessAndPackageContracts) {
  const std::string cmakeSource = readRepoFile("native/CMakeLists.txt");
  const std::string adapterHeader = readRepoFile("native/src/modules/ZoomMeetingSdkAdapter.h");
  const std::string readinessSource = readRepoFile("src/engine/zoomSdkReadiness.ts");
  const std::string packageSource = readRepoFile("src/engine/zoomWindowsSdkPackage.ts");
  ASSERT_FALSE(cmakeSource.empty());
  ASSERT_FALSE(adapterHeader.empty());
  ASSERT_FALSE(readinessSource.empty());
  ASSERT_FALSE(packageSource.empty());

  EXPECT_NE(cmakeSource.find("COREVIDEO_WITH_ZOOM"), std::string::npos);
  EXPECT_NE(cmakeSource.find("COREVIDEO_WITH_D3D11"), std::string::npos);
  EXPECT_NE(cmakeSource.find("COREVIDEO_WITH_MF_ENCODER"), std::string::npos);
  EXPECT_NE(cmakeSource.find("COREVIDEO_ENABLE_DEV_ADAPTERS"), std::string::npos);
  EXPECT_NE(cmakeSource.find("COREVIDEO_ZOOM_SDK_ROOT"), std::string::npos);
  EXPECT_NE(adapterHeader.find("IZoomMeetingSdkCaptureSource"), std::string::npos);
  EXPECT_NE(adapterHeader.find("IZoomCaptureSource"), std::string::npos);

  const std::array<std::string_view, 7> requiredPackageFiles = {
      "bin/sdk.dll",
      "lib/sdk.lib",
      "h/zoom_sdk.h",
      "h/meeting_service_interface.h",
      "h/rawdata/zoom_rawdata_api.h",
      "h/rawdata/rawdata_renderer_interface.h",
      "h/rawdata/rawdata_audio_helper_interface.h",
  };
  expectAllStringsPresent(cmakeSource + packageSource, requiredPackageFiles);

  const std::array<std::string_view, 7> readinessChecks = {
      "sdk-runtime",
      "app-key",
      "oauth",
      "jwt-broker",
      "raw-video",
      "raw-audio",
      "raw-share",
  };
  expectAllStringsPresent(readinessSource, readinessChecks);
}
