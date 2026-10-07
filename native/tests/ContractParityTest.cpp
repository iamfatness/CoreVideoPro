#include "core/MediaCore.h"
#include "core/Protocol.h"
#include "contracts/Lifecycle.h"
#include "modules/Interfaces.h"
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

template <typename Strings>
std::set<std::string> toSet(const Strings& strings) {
  std::set<std::string> result;
  for (const auto value : strings) {
    result.insert(std::string(value));
  }
  return result;
}

std::set<std::string> matches(const std::string& source, const std::string& pattern) {
  const std::regex expression(pattern);
  std::set<std::string> result;
  for (auto it = std::sregex_iterator(source.begin(), source.end(), expression);
       it != std::sregex_iterator(); ++it) {
    result.insert((*it)[1].str());
  }
  return result;
}

// Concatenates every file with the given extension under a repo directory,
// skipping build output and, optionally, one file name.
std::string readRepoTree(const std::string& relativeDirectory, const std::string& extension,
                         const std::string& skipFileName = {}) {
  const std::filesystem::path root = std::filesystem::path(COREVIDEO_REPO_ROOT) / relativeDirectory;
  std::string result;
  std::error_code error;
  for (auto it = std::filesystem::recursive_directory_iterator(root, error);
       !error && it != std::filesystem::recursive_directory_iterator(); it.increment(error)) {
    const auto name = it->path().filename().string();
    if (it->is_directory() && (name == "bin" || name == "obj" || name == ".build")) {
      it.disable_recursion_pending();
      continue;
    }
    if (!it->is_regular_file() || it->path().extension() != extension || name == skipFileName) {
      continue;
    }
    std::ifstream input(it->path());
    std::ostringstream buffer;
    buffer << input.rdbuf();
    result += buffer.str();
    result += '\n';
  }
  return result;
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
    else if (name == "DeliveryEvidenceObservation") valid = validateDeliveryEvidenceObservation(*payload);
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

// Cross-language parity for the manifests in core/Protocol.h (#747). The
// TypeScript mirrors these used to be compared against are gone (#738); the
// comparisons below are against what the core actually does and what the C#
// and Swift shells actually send and parse.

TEST(ContractParity, CapabilityManifestIsExactlyWhatTheCoreReports) {
  corevideo::core::MediaCore core(corevideo::modules::createStubModules());
  const auto profile = core.profile();
  const auto* states = profile.get("capabilityStates");
  ASSERT_NE(states, nullptr);
  ASSERT_TRUE(states->isObject());
  std::set<std::string> reported;
  for (const auto& entry : states->asObject()) {
    reported.insert(entry.first);
  }
  const auto manifest = toSet(corevideo::core::kNativeMediaCoreCapabilities);
  EXPECT_EQ(manifest.size(), corevideo::core::kNativeMediaCoreCapabilities.size()) << "duplicate capability";
  EXPECT_EQ(reported, manifest);
}

TEST(ContractParity, RequiredCapabilitiesMatchTheShellValidator) {
  const std::string source = readRepoFile("native-shell/CoreVideoPro.MediaCore/Models/NativeMediaCoreProtocol.cs");
  ASSERT_FALSE(source.empty());
  // The C# validator is what actually gates readiness on these names.
  const auto listStart = source.find("RequiredMvpCapabilities =");
  ASSERT_NE(listStart, std::string::npos);
  const auto listEnd = source.find("];", listStart);
  ASSERT_NE(listEnd, std::string::npos);
  const auto shellRequired = matches(source.substr(listStart, listEnd - listStart), "\"([^\"]+)\"");
  ASSERT_FALSE(shellRequired.empty());
  const auto required = toSet(corevideo::core::kRequiredMvpCapabilities);
  EXPECT_EQ(shellRequired, required);
  const auto capabilities = toSet(corevideo::core::kNativeMediaCoreCapabilities);
  for (const auto& name : required) {
    EXPECT_TRUE(capabilities.contains(name)) << "required capability the core never reports: " << name;
  }
}

TEST(ContractParity, RequestManifestMatchesTheRpcDispatcher) {
  const std::string dispatcher = readRepoFile("native/src/rpc/JsonRpcServer.cpp");
  ASSERT_FALSE(dispatcher.empty());
  const auto handled = matches(dispatcher, "hasType\\(request,\\s*\"([^\"]+)\"\\)");
  ASSERT_FALSE(handled.empty());
  const auto manifest = toSet(corevideo::core::kCoreRequestTypes);
  EXPECT_EQ(manifest.size(), corevideo::core::kCoreRequestTypes.size()) << "duplicate request type";
  EXPECT_EQ(handled, manifest);
}

TEST(ContractParity, EveryRequestTheShellsSendIsHandledByTheCore) {
  const auto requests = toSet(corevideo::core::kCoreRequestTypes);
  const auto commands = toSet(corevideo::core::kNativeMediaCoreCommandTypes);

  // C#: requests are built as dictionaries with ["type"] = "<request>".
  std::set<std::string> csharpRequests;
  for (const char* directory : {"native-shell/CoreVideoPro.MediaCore", "native-shell/CoreVideoPro.WinUI",
                                "native-shell/CoreVideoPro.Control"}) {
    const auto found = matches(readRepoTree(directory, ".cs"), "\\[\"type\"\\]\\s*=\\s*\"([a-z0-9-]+)\"");
    csharpRequests.insert(found.begin(), found.end());
  }
  ASSERT_FALSE(csharpRequests.empty());
  for (const auto& name : csharpRequests) {
    EXPECT_TRUE(requests.contains(name)) << "C# shell sends a request the core does not handle: " << name;
  }

  // Swift: "type": "<name>" appears on requests and on the commands inside a
  // media-core-sync batch, so either manifest satisfies it.
  const auto swiftTypes = matches(
      readRepoTree("mac-shell/Sources/CoreVideoProShell", ".swift", "ShellTests.swift"),
      "\"type\":\\s*\"([a-z0-9-]+)\"");
  ASSERT_FALSE(swiftTypes.empty());
  for (const auto& name : swiftTypes) {
    EXPECT_TRUE(requests.contains(name) || commands.contains(name))
        << "Swift shell sends a type the core does not handle: " << name;
  }
}

TEST(ContractParity, EveryCoreEventIsEmittedByTheCoreAndParsedByTheShell) {
  const std::string coreSource = readRepoTree("native/src", ".cpp");
  const std::string shellParser =
      readRepoFile("native-shell/CoreVideoPro.MediaCore/Services/CoreProtocolParser.cs") +
      readRepoFile("native-shell/CoreVideoPro.MediaCore/Models/CoreProtocolModels.cs");
  ASSERT_FALSE(coreSource.empty());
  ASSERT_FALSE(shellParser.empty());
  for (const auto name : corevideo::core::kCoreEventTypes) {
    const std::string emit = "{\"type\", \"" + std::string(name) + "\"}";
    EXPECT_NE(coreSource.find(emit), std::string::npos) << "manifest event the core never emits: " << name;
    const std::string quoted = "\"" + std::string(name) + "\"";
    EXPECT_NE(shellParser.find(quoted), std::string::npos) << "core event the C# shell does not parse: " << name;
  }
}

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
