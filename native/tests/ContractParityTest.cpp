#include "core/Protocol.h"
#include "contracts/Lifecycle.h"
#include "rpc/BoundedResponseLane.h"

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

TEST(ContractParity, LifecycleGoldenMessagesMatchSchema) {
  const auto fixtures = corevideo::rpc::Json::parse(readRepoFile("contracts/lifecycle.fixtures.json"));
  ASSERT_TRUE(fixtures && fixtures->isArray());
  ASSERT_FALSE(fixtures->asArray().empty());
  for (const auto& fixture : fixtures->asArray()) {
    const auto payload = corevideo::rpc::Json::parse(fixture.getString("json"));
    ASSERT_TRUE(payload);
    const auto name = fixture.getString("contract");
    bool valid = false;
    using namespace corevideo::contracts;
    if (name == "ProtocolVersion") valid = validateProtocolVersion(*payload);
    else if (name == "OutputLifecycle") valid = validateOutputLifecycle(*payload);
    else if (name == "OperationStatus") valid = validateOperationStatus(*payload);
    else if (name == "ProtocolFailure") valid = validateProtocolFailure(*payload);
    else ASSERT_TRUE(false) << "Unknown contract: " << name;
    ASSERT_NE(fixture.get("accepted"), nullptr);
    EXPECT_EQ(valid, fixture.get("accepted")->asBool()) << fixture.getString("id");
  }
  const corevideo::contracts::OutputLifecycle lifecycle{"session-1", true, "starting", "unknown", false, std::nullopt};
  const auto wire = corevideo::contracts::toJson(lifecycle);
  EXPECT_TRUE(corevideo::contracts::validateOutputLifecycle(wire));
  EXPECT_EQ(wire.get("error"), nullptr);
}

TEST(ContractParity, IdentityGoldenMessagesMatchSchema) {
  const auto fixtures = corevideo::rpc::Json::parse(readRepoFile("contracts/identity.fixtures.json"));
  ASSERT_TRUE(fixtures && fixtures->isArray());
  ASSERT_FALSE(fixtures->asArray().empty());
  for (const auto& fixture : fixtures->asArray()) {
    const auto payload = corevideo::rpc::Json::parse(fixture.getString("json"));
    ASSERT_TRUE(payload);
    const auto name = fixture.getString("contract");
    bool valid = false;
    using namespace corevideo::contracts;
    if (name == "EntityIdentity") valid = validateEntityIdentity(*payload);
    else if (name == "EntityRevision") valid = validateEntityRevision(*payload);
    else if (name == "SourceInstanceIdentity") valid = validateSourceInstanceIdentity(*payload);
    else if (name == "ParticipantBindingIdentity") valid = validateParticipantBindingIdentity(*payload);
    else if (name == "ControlRevision") valid = validateControlRevision(*payload);
    else if (name == "PlanGeneration") valid = validatePlanGeneration(*payload);
    else if (name == "ControlOperationIdentity") valid = validateControlOperationIdentity(*payload);
    else ASSERT_TRUE(false) << "Unknown identity contract: " << name;
    EXPECT_EQ(valid, fixture.get("accepted")->asBool()) << fixture.getString("id");
  }
  const corevideo::contracts::EntityRevision revision{"scene", "scene-1", "epoch-1", 9007199254740991LL};
  const auto wire = corevideo::contracts::toJson(revision);
  const auto reparsed = corevideo::rpc::Json::parse(wire.stringify());
  ASSERT_TRUE(reparsed);
  EXPECT_TRUE(corevideo::contracts::validateEntityRevision(*reparsed));
  EXPECT_EQ(reparsed->getNumber("revision"), 9007199254740991.0);
}

TEST(ContractParity, EvidenceGoldenMessagesMatchSchema) {
  const auto fixtures = corevideo::rpc::Json::parse(readRepoFile("contracts/evidence.fixtures.json"));
  ASSERT_TRUE(fixtures && fixtures->isArray());
  ASSERT_FALSE(fixtures->asArray().empty());
  for (const auto& fixture : fixtures->asArray()) {
    const auto payload = corevideo::rpc::Json::parse(fixture.getString("json"));
    ASSERT_TRUE(payload);
    const auto name = fixture.getString("contract");
    bool valid = false;
    using namespace corevideo::contracts;
    if (name == "AcceptedOperationObservation") valid = validateAcceptedOperationObservation(*payload);
    else if (name == "AppliedOperationObservation") valid = validateAppliedOperationObservation(*payload);
    else if (name == "RenderedMediaObservation") valid = validateRenderedMediaObservation(*payload);
    else if (name == "DeliveredMediaObservation") valid = validateDeliveredMediaObservation(*payload);
    else if (name == "PresentedMediaObservation") valid = validatePresentedMediaObservation(*payload);
    else if (name == "MuxedMediaObservation") valid = validateMuxedMediaObservation(*payload);
    else if (name == "CommittedMediaObservation") valid = validateCommittedMediaObservation(*payload);
    else if (name == "CompletedOutputObservation") valid = validateCompletedOutputObservation(*payload);
    else if (name == "ResourceLeaseDescriptor") valid = validateResourceLeaseDescriptor(*payload);
    else if (name == "DestinationProgress") valid = validateDestinationProgress(*payload);
    else if (name == "ArtifactValidationResult") valid = validateArtifactValidationResult(*payload);
    else ASSERT_TRUE(false) << "Unknown identity contract: " << name;
    EXPECT_EQ(valid, fixture.get("accepted")->asBool()) << fixture.getString("id");
  }
}

TEST(ContractParity, ProductionBuilderCommandsAreInTheLiveDispatcher) {
  const std::string builder = readRepoFile("native-shell/CoreVideoPro.MediaCore/Services/MediaCoreCommandBuilder.cs");
  const std::string dispatcher = readRepoFile("native/src/core/MediaCore.cpp");
  ASSERT_FALSE(builder.empty());
  ASSERT_FALSE(dispatcher.empty());
  std::size_t cursor = 0;
  while ((cursor = builder.find("Command(\"", cursor)) != std::string::npos) {
    cursor += std::string("Command(\"").size();
    const auto end = builder.find('"', cursor);
    ASSERT_NE(end, std::string::npos);
    const auto name = builder.substr(cursor, end - cursor);
    EXPECT_TRUE(corevideo::core::isNativeMediaCoreCommand(name)) << name;
    cursor = end + 1;
  }
  for (const auto name : corevideo::core::kNativeMediaCoreCommandTypes) {
    const auto needle = std::string("type == \"") + std::string(name) + "\"";
    EXPECT_NE(dispatcher.find(needle), std::string::npos) << name;
  }
}

TEST(ContractParity, ResponseLaneDropsOldestWhenFull) {
  corevideo::rpc::BoundedResponseLane lane;
  for (std::size_t i = 0; i < corevideo::rpc::BoundedResponseLane::kMaxDepth + 3; ++i) {
    lane.push(std::to_string(i));
  }
  EXPECT_EQ(lane.size(), corevideo::rpc::BoundedResponseLane::kMaxDepth);
  EXPECT_EQ(lane.dropped(), 3u);
  EXPECT_EQ(lane.popFront().first, "3");
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

TEST(ContractParity, CoreEventTypesMatchNativeProtocolMirror) {
  const std::string nodeProtocol = readRepoFile("native-core/src/protocol.ts");
  ASSERT_FALSE(nodeProtocol.empty());
  expectAllStringsPresent(nodeProtocol, corevideo::core::kCoreEventTypes);
  EXPECT_NE(nodeProtocol.find("participantId"), std::string::npos);
  EXPECT_NE(nodeProtocol.find("frameId"), std::string::npos);
}

TEST(ContractParity, ZoomMediaSpineSyncRequestTypeIsMirrored) {
  const std::string nodeProtocol = readRepoFile("native-core/src/protocol.ts");
  ASSERT_FALSE(nodeProtocol.empty());

  expectAllStringsPresent(nodeProtocol, corevideo::core::kCoreRequestTypes);
  EXPECT_NE(nodeProtocol.find("zoom-media-spine-sync"), std::string::npos);
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
  EXPECT_NE(cmakeSource.find("COREVIDEO_WITH_RTMP_OUTPUT"), std::string::npos);
  EXPECT_NE(cmakeSource.find("COREVIDEO_WITH_DECKLINK"), std::string::npos);
  EXPECT_NE(cmakeSource.find("COREVIDEO_WITH_AJA"), std::string::npos);
  EXPECT_NE(cmakeSource.find("COREVIDEO_BUILD_ZOOM_ENGINE"), std::string::npos);
  EXPECT_NE(cmakeSource.find("COREVIDEO_ENABLE_DEV_ADAPTERS"), std::string::npos);
  EXPECT_NE(cmakeSource.find("COREVIDEO_ZOOM_SDK_ROOT"), std::string::npos);
  EXPECT_NE(cmakeSource.find("COREVIDEO_BUILD_ZOOM_ENGINE requires -DZOOM_SDK_DIR"), std::string::npos);
  EXPECT_NE(adapterHeader.find("IZoomMeetingSdkCaptureSource"), std::string::npos);
  EXPECT_NE(adapterHeader.find("takeVideoFrames"), std::string::npos);

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
