#include "core/Protocol.h"
#include "contracts/Lifecycle.h"
#include "rpc/BoundedResponseLane.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <regex>
#include <set>
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
    else if (name == "ZoomRosterSnapshotRevision") valid = validateZoomRosterSnapshotRevision(*payload);
    else if (name == "ZoomRosterParticipantFact") valid = validateZoomRosterParticipantFact(*payload);
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
  // Extract the executable branch names from the shipping C++ dispatcher.
  // The manifest and shell builder must agree with those branches in both
  // directions, so adding an unlisted branch or a phantom manifest entry fails.
  const std::regex branchPattern("type\\s*==\\s*\"([^\"]+)\"");
  std::set<std::string> liveBranches;
  for (auto it = std::sregex_iterator(dispatcher.begin(), dispatcher.end(), branchPattern);
       it != std::sregex_iterator(); ++it) {
    liveBranches.insert((*it)[1].str());
  }
  ASSERT_FALSE(liveBranches.empty());
  std::set<std::string> manifest;
  for (const auto name : corevideo::core::kNativeMediaCoreCommandTypes) {
    EXPECT_TRUE(manifest.insert(std::string(name)).second) << "duplicate manifest command: " << name;
  }
  EXPECT_EQ(liveBranches, manifest);

  const std::regex builderPattern("Command\\(\"([^\"]+)\"");
  std::set<std::string> productionCommands;
  for (auto it = std::sregex_iterator(builder.begin(), builder.end(), builderPattern);
       it != std::sregex_iterator(); ++it) {
    productionCommands.insert((*it)[1].str());
  }
  ASSERT_FALSE(productionCommands.empty());
  for (const auto& name : productionCommands) {
    EXPECT_TRUE(liveBranches.contains(name)) << "production command has no live dispatcher branch: " << name;
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

// The TypeScript-mirror parity tests (capability strings, bridge envelope
// types, Zoom media-spine names, core event/request types) were retired with
// the React prototype and Node core simulator they compared against (#738).
// The manifests in core/Protocol.h are still checked against the shipping
// dispatcher and C# builder above.

TEST(ContractParity, ZoomMeetingSdkAdapterGateIsDeclaredInTheBuild) {
  const std::string cmakeSource = readRepoFile("native/CMakeLists.txt");
  const std::string adapterHeader = readRepoFile("native/src/modules/ZoomMeetingSdkAdapter.h");
  ASSERT_FALSE(cmakeSource.empty());
  ASSERT_FALSE(adapterHeader.empty());

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

  // The SDK package files the build itself links and stages.
  const std::array<std::string_view, 2> requiredPackageFiles = {
      "bin/sdk.dll",
      "lib/sdk.lib",
  };
  expectAllStringsPresent(cmakeSource, requiredPackageFiles);
}
