// The app's own virtual camera is never a capture source (review M3), and a
// refused producer reads as CAMERA_PRODUCER_REFUSED, not a bare -4 (review I1).
#include "modules/OwnVirtualCamera.h"
#include "modules/SinkVirtualCameraPublisher.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

using namespace corevideo::modules;

TEST(OwnVirtualCamera, MatchesTheExtensionsDeviceByUidOrDefaultName) {
  EXPECT_TRUE(isOwnVirtualCamera("us.iamfatness.corevideopro.camera.device", "Studio Cam"));  // renamed, legacy UID
  EXPECT_TRUE(isOwnVirtualCamera("791e98ed-72e7-4716-9a26-1b4fbb9a1520", "Studio Cam"));      // UUID, any case
  EXPECT_TRUE(isOwnVirtualCamera("some-other-uid", "CoreVideo Pro Camera"));
  EXPECT_FALSE(isOwnVirtualCamera("1F06D5FF-4C76-410E-9770-C04C467C1317", "FaceTime HD Camera"));
  EXPECT_FALSE(isOwnVirtualCamera("DFCE4674-D8B4-E9C2-D625-339240874FB3", "OBSBOT Virtual Camera"));
  EXPECT_TRUE(isOwnVirtualCameraUid("us.iamfatness.corevideopro.camera.device"));
  EXPECT_FALSE(isOwnVirtualCameraUid("CoreVideo Pro Camera"));
}

// The ids are declared twice (Swift for the extension, C++ for the core); keep them one.
TEST(OwnVirtualCamera, IdsMatchTheExtensionsCameraConstants) {
  std::ifstream in(std::filesystem::path(COREVIDEO_REPO_ROOT) / "mac-shell" / "Sources" / "CameraExtensionKit" /
                   "CameraConstants.swift");
  ASSERT_TRUE(in.good());
  std::stringstream text;
  text << in.rdbuf();
  const std::string swift = text.str();
  EXPECT_NE(swift.find(std::string("UUID(uuidString: \"") + kOwnVirtualCameraUuid + "\")!\n"), std::string::npos)
      << "deviceUUID";
  EXPECT_NE(swift.find(std::string("legacyDeviceID = \"") + kOwnVirtualCameraLegacyId + "\""), std::string::npos);
  EXPECT_NE(swift.find(std::string("defaultDeviceName = \"") + kOwnVirtualCameraDefaultName + "\""), std::string::npos);
}

TEST(SinkStartStreamWarning, ARefusedProducerSaysWhatToDo) {
  const std::string refused = sinkStartStreamWarning(-4);
  EXPECT_EQ(refused.rfind("CAMERA_PRODUCER_REFUSED:", 0), 0u) << refused;
  EXPECT_NE(refused.find("Applications folder"), std::string::npos);
  // Still CAMERA_-prefixed, so the 1 s retry continues.
  EXPECT_EQ(refused.rfind("CAMERA_", 0), 0u);
  EXPECT_EQ(sinkStartStreamWarning(-50), "CAMERA_SINK_FAILED: CMIODeviceStartStream failed (OSStatus -50)");
}
