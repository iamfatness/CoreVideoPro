#include "core/MediaCore.h"
#include "rpc/JsonRpcServer.h"
#include "gtest/gtest.h"
#include "contracts/Lifecycle.h"
#include <chrono>
#include <thread>

#if COREVIDEO_WITH_D3D11
TEST(GraphicsRpcPixel, CommandsReachCoreStateAndActualProgramPixels) {
  auto modules = corevideo::modules::createStubModules();
  modules.compositor = corevideo::modules::createD3D11Compositor();
  ASSERT_NE(modules.compositor, nullptr);
  corevideo::core::MediaCore core(std::move(modules));
  corevideo::rpc::JsonRpcServer server(core);
  using corevideo::rpc::Json;
  int sequence = 0;
  const auto state = [&] {
    auto response = server.handle(Json::Object{{"id", "tick"}, {"type", "media-core-sync"}, {"commands", Json::Array{}}});
    EXPECT_TRUE(response.get("ok") && response.get("ok")->asBool());
    ASSERT_NE(response.get("snapshot"), nullptr);
    return *response.get("snapshot");
  };
  const auto initial = state();
  const auto epoch = initial.get("graphics")->getString("coreEpoch");
  ASSERT_FALSE(epoch.empty());
  const auto command = [&](const std::string& type, Json::Object payload) {
    const auto operation = "operation-" + std::to_string(++sequence);
    payload["schemaVersion"] = 1; payload["requestId"] = operation;
    payload["clientId"] = "rpc-qa"; payload["coreEpoch"] = epoch; payload["instanceId"] = "featured";
    auto response = server.handle(Json::Object{{"id", operation}, {"type", type}, {"graphics", std::move(payload)}});
    EXPECT_TRUE(response.get("ok") && response.get("ok")->asBool());
    const auto* snapshot = response.get("snapshot");
    ASSERT_NE(snapshot, nullptr);
    const auto* result = snapshot->get("graphics")->get("lastResult");
    EXPECT_EQ(operation, result->getString("requestId"));
    EXPECT_TRUE(result->get("accepted")->asBool()) << result->getString("reason");
    return *snapshot;
  };
  command("graphics-instance-create", {{"templateId", "compact-solid"}, {"templateVersion", 1}, {"scope", "global"},
      {"fields", Json::Object{{"name", "Native RPC guest"}, {"title", "Engineer"}}}});
  command("graphics-instance-cue", {{"expectedRevision", 1}});
  command("graphics-instance-trigger", {{"expectedRevision", 1}, {"action", "show"}});
  Json onAir;
  const auto limit = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (std::chrono::steady_clock::now() < limit) {
    onAir = state();
    if (onAir.get("graphics")->get("instances")->asArray().front().getNumber("firstDrawSubmittedFrame") > 0) break;
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  const auto& instance = onAir.get("graphics")->get("instances")->asArray().front();
  EXPECT_GT(instance.getNumber("firstDrawSubmittedFrame"), 0.);
  ASSERT_NE(instance.get("preparationObservation"), nullptr);
  ASSERT_NE(instance.get("firstCompositionObservation"), nullptr);
  EXPECT_TRUE(corevideo::contracts::validateGraphicsPreparationFact(*instance.get("preparationObservation")));
  EXPECT_TRUE(corevideo::contracts::validateGraphicsCompositionFact(*instance.get("firstCompositionObservation")));
  ASSERT_NE(onAir.get("previewScene"), nullptr);
  EXPECT_TRUE(onAir.get("previewScene")->get("composite")->asBool());
  EXPECT_EQ(onAir.get("previewScene")->getNumber("layerCount"), 1.);
  ASSERT_NE(onAir.get("programFramePreview"), nullptr);
  const auto pixels = onAir.get("programFramePreview")->getString("bgraBase64");
  ASSERT_FALSE(pixels.empty());
  const auto cleared = command("graphics-program-clear", {{"channel", "all"}});
  EXPECT_EQ("hidden", cleared.get("graphics")->get("instances")->asArray().front().getString("visibility"));
  ASSERT_NE(cleared.get("programFramePreview"), nullptr);
  EXPECT_NE(pixels, cleared.get("programFramePreview")->getString("bgraBase64"));
}

#endif
