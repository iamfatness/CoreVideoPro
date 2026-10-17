#include "core/MediaCore.h"
#include "rpc/JsonRpcServer.h"
#include "gtest/gtest.h"
#include "contracts/Lifecycle.h"
#include "graphics/GraphicsResourceBudget.h"
#include "graphics/GraphicsTemplate.h"
#include "GraphicsAssetFixture.h"
#include <algorithm>
#include <array>
#include <chrono>
#include <thread>

#if COREVIDEO_WITH_D3D11
TEST(GraphicsCompositionPixel, EveryCuratedLookReachesActualNativeProgramPixels) {
  using corevideo::rpc::Json;
  for (const auto& definition:corevideo::graphics::GraphicsTemplate::curated()) {
    double now=0; corevideo::graphics::GraphicsInstances state{"test-core",[&]{return now;}};
    auto compositor=corevideo::modules::createD3D11Compositor(); ASSERT_TRUE(compositor); int sequence=0;
    const auto command=[&](const std::string& type,Json::Object fields) {
      fields["schemaVersion"]=1; fields["coreEpoch"]="test-core"; fields["clientId"]="curated-qa"; fields["instanceId"]="featured";
      fields["requestId"]="curated-"+std::to_string(++sequence); fields["expectedRevision"]=1;
      return state.apply(Json::Object{{"type",type},{"graphics",std::move(fields)}});
    };
    const auto render=[&] {
      corevideo::modules::CompositorRenderPlan plan; plan.width=640; plan.height=360;
      corevideo::modules::CompositorRenderPlanLayer background; background.kind="background"; background.hasFillColor=true; background.fillColor="#203060";
      plan.layers.push_back(background); state.append(plan,false); auto frame=compositor->render(plan,{}); state.observe(frame); return frame.preview.bgra;
    };
    const auto baseline=render();
    ASSERT_TRUE(command("graphics-instance-create",{{"templateId",definition->id()},{"templateVersion",1},{"scope","global"},{"fields",definition->id()=="corporate-lower-third" ? Json{Json::Object{{"name","Ada"},{"title","Engineer"}}} : definition->defaultFields()}}));
    const auto until=std::chrono::steady_clock::now()+std::chrono::seconds(5);
    bool requested=false; std::vector<uint8_t> pixels;
    do {
      render(); const auto row=state.snapshot().get("instances")->asArray().front();
      if(!requested && row.getString("preparation")!="waiting-clock") { ASSERT_TRUE(command("graphics-instance-trigger",{{"action","show"}})); requested=true; }
      now+=16.667; pixels=render();
      if(row.getNumber("firstDrawSubmittedFrame")>0 && now>1500) break;
      std::this_thread::sleep_for(std::chrono::milliseconds(2));
    } while(std::chrono::steady_clock::now()<until);
    ASSERT_EQ(baseline.size(),pixels.size()); ASSERT_FALSE(pixels.empty());
    size_t changed=0; for(size_t i=0;i<pixels.size();i+=4) if(!std::equal(pixels.begin()+i,pixels.begin()+i+3,baseline.begin()+i)) ++changed;
    EXPECT_GT(changed,32u); EXPECT_GT(state.snapshot().get("instances")->asArray().front().getNumber("firstDrawSubmittedFrame"),0.);
  }
}

TEST(GraphicsCompositionPixel, VerifiedLogoInCompatibilityLookChangesActualProgramAndPrivatePreviewUsesSameBytes) {
 using corevideo::rpc::Json;corevideo::graphics::GraphicsInstances graphics{"test-core"};auto compositor=corevideo::modules::createD3D11Compositor();ASSERT_TRUE(compositor);
 int sequence=0;const auto command=[&](const std::string& type,Json::Object body) {body["schemaVersion"]=1;body["coreEpoch"]="test-core";body["clientId"]="logo-compat-qa";body["requestId"]="logo-"+std::to_string(++sequence);body["instanceId"]="logo";if(!body.count("expectedRevision")) body["expectedRevision"]=1;return graphics.apply(Json::Object{{"type",type},{"graphics",body}});};
 const auto row=[&]() -> const Json& {return graphics.snapshot().get("instances")->asArray().front();};
 const auto render=[&] {corevideo::modules::CompositorRenderPlan plan;plan.width=640;plan.height=360;graphics.append(plan,false);auto frame=compositor->render(plan,{});graphics.observe(frame);return frame.preview.bgra;};
 ASSERT_TRUE(command("graphics-instance-create",{{"templateId","compact-solid"},{"templateVersion",1},{"scope","global"},{"fields",Json::Object{{"name","Guest"}}}}));ASSERT_TRUE(command("graphics-instance-trigger",{{"action","show"}}));
 const auto until=[&](uint64_t revision) {const auto end=std::chrono::steady_clock::now()+std::chrono::seconds(5);while((row().getNumber("programRevision")!=revision || row().getNumber("firstDrawSubmittedFrame")==0) && std::chrono::steady_clock::now()<end) {render();std::this_thread::sleep_for(std::chrono::milliseconds(2));}return row().getNumber("programRevision")==revision && row().getNumber("firstDrawSubmittedFrame")>0;};
 ASSERT_TRUE(until(1));const auto clean=render();
 Json image=Json::Object{{"templateId","corner-logo"},{"templateVersion",1},{"assetId","logo"}};
 ASSERT_TRUE(command("graphics-instance-update",{{"target","program"},{"image",image}}));ASSERT_TRUE(until(2));EXPECT_NE(clean,render());
 const auto privateContent=graphics.authoringPreview(Json::Object{{"templateId","compact-solid"},{"templateVersion",1},{"fields",Json::Object{{"name","Guest"}}},{"image",image}});
 ASSERT_TRUE(privateContent);corevideo::modules::CompositorRenderPlan plan;plan.width=640;plan.height=360;graphics.append(plan,false);ASSERT_EQ(plan.layers.size(),1u);
 EXPECT_EQ(privateContent->graphicsImage,plan.layers[0].overlay.graphicsImage);EXPECT_TRUE(privateContent->graphicsImage);
}

TEST(GraphicsCompositionPixel, ClockTicksAdoptPreparedDigitsWithoutBlankingProgramOrChangingEditRevision) {
  using corevideo::rpc::Json;
  double now=0; corevideo::graphics::GraphicsInstances state{"test-core",[&]{return now;}};
  auto compositor=corevideo::modules::createD3D11Compositor(); ASSERT_TRUE(compositor); int sequence=0;
  const auto command=[&](const std::string& type,Json::Object fields) {
    fields["schemaVersion"]=1; fields["coreEpoch"]="test-core"; fields["clientId"]="clock-pixel-qa"; fields["instanceId"]="clock";
    fields["requestId"]="clock-pixel-"+std::to_string(++sequence); fields["expectedRevision"]=1;
    return state.apply(Json::Object{{"type",type},{"graphics",std::move(fields)}});
  };
  const auto render=[&] {
    corevideo::modules::CompositorRenderPlan plan; plan.width=640; plan.height=360;
    corevideo::modules::CompositorRenderPlanLayer background; background.kind="background"; background.hasFillColor=true; background.fillColor="#203060";
    plan.layers.push_back(background); state.append(plan,false); auto frame=compositor->render(plan,{}); state.observe(frame); now+=16.667; return frame.preview.bgra;
  };
  const auto baseline=render(); ASSERT_TRUE(command("graphics-instance-create",{{"templateId","clock-panel"},{"templateVersion",1},{"scope","global"}}));
  const auto until=std::chrono::steady_clock::now()+std::chrono::seconds(8);
  bool requested=false;
  while(std::chrono::steady_clock::now()<until) {
    render(); const auto item=state.snapshot().get("instances")->asArray().front();
    if(!requested && item.getString("preparation")!="waiting-clock") { ASSERT_TRUE(command("graphics-instance-trigger",{{"action","show"}})); requested=true; }
    if(item.getNumber("firstDrawSubmittedFrame")>0 && now>=1000) break;
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  const auto first=state.snapshot().get("instances")->asArray().front(); ASSERT_GT(first.getNumber("firstDrawSubmittedFrame"),0.);
  const auto initialTime=first.get("programFields")->getString("time");
  const auto end=std::chrono::steady_clock::now()+std::chrono::milliseconds(1600); bool changed=false; int measured=0;
  while(std::chrono::steady_clock::now()<end) {
    const auto pixels=render(); ASSERT_EQ(baseline.size(),pixels.size()); EXPECT_NE(baseline,pixels);
    const auto item=state.snapshot().get("instances")->asArray().front();
    EXPECT_TRUE(item.get("currentlyComposed")->asBool()); EXPECT_EQ(1,item.getNumber("revision")); EXPECT_EQ(1,item.getNumber("programRevision"));
    changed=changed || item.get("programFields")->getString("time")!=initialTime; ++measured;
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  EXPECT_TRUE(changed); EXPECT_GT(measured,20);
}

TEST(GraphicsCompositionPixel, VerifiedPackageImagePreservesAlphaAndAcceptedBytesAfterFileChanges) {
  using corevideo::rpc::Json;
  std::string error;
  const auto asset = corevideo::graphics::graphicsAssetFromBytes("logo", graphicsTest::png(), error); ASSERT_TRUE(asset);
  graphicsTest::Package package(asset); const auto rootBytes = package.root.u8string(); const std::string root(rootBytes.begin(),rootBytes.end());
  corevideo::graphics::GraphicsInstances graphics{"test-core"};
  auto compositor = corevideo::modules::createD3D11Compositor(); ASSERT_TRUE(compositor);
  int sequence = 0;
  const auto command = [&](const std::string& type, Json::Object payload) {
    payload["schemaVersion"] = 1; payload["requestId"] = "image-request-" + std::to_string(++sequence);
    payload["clientId"] = "image-qa"; payload["coreEpoch"] = "test-core"; payload["instanceId"] = "logo";
    return graphics.apply(Json::Object{{"type",type},{"graphics",std::move(payload)}});
  };
  const auto render = [&] {
    corevideo::modules::CompositorRenderPlan plan; plan.width = 320; plan.height = 180;
    corevideo::modules::CompositorRenderPlanLayer background; background.kind = "background"; background.hasFillColor = true; background.fillColor = "#0000FF";
    plan.layers.push_back(background); graphics.append(plan,false);
    auto frame = compositor->render(plan,{}); graphics.observe(frame); return frame.preview.bgra;
  };
  ASSERT_TRUE(command("graphics-template-install", {{"manifest",graphicsTest::imageDocument(asset).stringify()},{"packageRoot",root}}));
  const auto importLimit = std::chrono::steady_clock::now()+std::chrono::seconds(5);
  while (graphics.snapshot().get("templates")->asArray().size() != 10 && std::chrono::steady_clock::now()<importLimit) { render(); std::this_thread::sleep_for(std::chrono::milliseconds(2)); }
  ASSERT_EQ(10u,graphics.snapshot().get("templates")->asArray().size());
  ASSERT_TRUE(command("graphics-instance-create", {{"templateId","packaged-logo"},{"templateVersion",1},{"scope","global"}}));
  ASSERT_TRUE(command("graphics-instance-trigger", {{"expectedRevision",1},{"action","show"}}));
  const auto limit = std::chrono::steady_clock::now()+std::chrono::seconds(5);
  while (graphics.snapshot().get("instances")->asArray().front().getNumber("firstDrawSubmittedFrame") == 0 && std::chrono::steady_clock::now()<limit) { render(); std::this_thread::sleep_for(std::chrono::milliseconds(2)); }
  ASSERT_GT(graphics.snapshot().get("instances")->asArray().front().getNumber("firstDrawSubmittedFrame"),0.);
  const auto pixels = render(); ASSERT_EQ(size_t(320*180*4),pixels.size());
  const auto left = (154*320+23)*4, right = (154*320+68)*4;
  EXPECT_GT(unsigned(pixels[left+2]),240u); EXPECT_LT(unsigned(pixels[left]),20u);
  EXPECT_LT(unsigned(pixels[right+2]),20u); EXPECT_NEAR(128.,double(pixels[right+1]),8.); EXPECT_NEAR(127.,double(pixels[right]),8.);
  { std::ofstream changed(package.file,std::ios::binary|std::ios::trunc); changed << "invalid replacement"; }
  EXPECT_EQ(pixels,render());
  EXPECT_GT(graphics.snapshot().getNumber("encodedTemplateAssetBytes"),0.);
}

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

  command("graphics-instance-remove", {{"expectedRevision", 1}});
  auto importedManifest = corevideo::graphics::GraphicsTemplate::corporateLowerThird()->document().asObject();
  importedManifest["templateId"] = "rpc-imported-lower-third"; importedManifest["templateVersion"] = 2;
  command("graphics-template-install", {{"manifest", Json(importedManifest).stringify()}});
  Json installed;
  const auto importLimit = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  do {
    installed = state();
    const auto& imports = installed.get("graphics")->get("templateInstalls")->asArray();
    if (!imports.empty() && imports.back().getString("state") == "installed") break;
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  } while (std::chrono::steady_clock::now() < importLimit);
  ASSERT_EQ(10u, installed.get("graphics")->get("templates")->asArray().size());
  const auto generic = command("graphics-instance-create", {{"templateId", "rpc-imported-lower-third"}, {"templateVersion", 2}, {"scope", "global"},
      {"fields", Json::Object{{"name", "Corporate native — 日本語"}, {"title", ""}}},
      {"animations", Json::Object{{"in", Json::Object{{"type", "none"}}}, {"out", Json::Object{{"type", "none"}}}}}});
  EXPECT_EQ(10u, generic.get("graphics")->get("templates")->asArray().size());
  EXPECT_EQ(2., generic.get("graphics")->get("instances")->asArray().front().getNumber("templateVersion"));
  command("graphics-instance-cue", {{"expectedRevision", 2}});
  const auto cueLimit = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  Json cued;
  while (std::chrono::steady_clock::now() < cueLimit) {
    cued = state();
    if (cued.get("graphics")->get("instances")->asArray().front().getNumber("cueRevision") == 2) break;
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  EXPECT_EQ(2., cued.get("graphics")->get("instances")->asArray().front().getNumber("cueRevision"));
  auto shown = command("graphics-instance-trigger", {{"expectedRevision", 2}, {"target", "cue"}, {"action", "show"}});
  const auto drawLimit = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (shown.get("graphics")->get("instances")->asArray().front().getNumber("firstDrawSubmittedFrame") == 0 &&
      std::chrono::steady_clock::now() < drawLimit) shown = state();
  const auto& genericRow = shown.get("graphics")->get("instances")->asArray().front();
  EXPECT_GT(genericRow.getNumber("firstDrawSubmittedFrame"), 0.);
  EXPECT_EQ("Corporate native — 日本語", genericRow.get("programFields")->getString("name"));
  EXPECT_EQ("", genericRow.get("programFields")->getString("title"));
  EXPECT_NE(cleared.get("programFramePreview")->getString("bgraBase64"), shown.get("programFramePreview")->getString("bgraBase64"));
  const auto firstGenericPixels = shown.get("programFramePreview")->getString("bgraBase64");
  auto glyphChange = command("graphics-instance-update", {{"expectedRevision", 2}, {"target", "program"},
      {"fields", Json::Object{{"name", "Different glyphs — العربية"}}}});
  const auto glyphLimit = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while ((glyphChange.get("graphics")->get("instances")->asArray().front().getNumber("programRevision") != 3 ||
      glyphChange.get("graphics")->get("instances")->asArray().front().getNumber("firstDrawSubmittedFrame") == 0) &&
      std::chrono::steady_clock::now() < glyphLimit) {
    glyphChange = state(); std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  EXPECT_EQ(3., glyphChange.get("graphics")->get("instances")->asArray().front().getNumber("programRevision"));
  EXPECT_NE(firstGenericPixels, glyphChange.get("programFramePreview")->getString("bgraBase64")); // text, not just a plate
  const auto validGlyphPixels = glyphChange.get("programFramePreview")->getString("bgraBase64");
  auto fontFailure = command("graphics-instance-update", {{"expectedRevision", 3}, {"target", "program"},
      {"style", Json::Object{{"fontFamily", "Missing Corporate Font 9c2a8f77"}}}});
  const auto fontLimit = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (fontFailure.get("graphics")->get("instances")->asArray().front().getString("preparation") != "failed" &&
      std::chrono::steady_clock::now() < fontLimit) {
    fontFailure = state(); std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  EXPECT_EQ("failed", fontFailure.get("graphics")->get("instances")->asArray().front().getString("preparation"));
  EXPECT_EQ(3., fontFailure.get("graphics")->get("instances")->asArray().front().getNumber("programRevision"));
  EXPECT_EQ(validGlyphPixels, fontFailure.get("programFramePreview")->getString("bgraBase64"));
}

TEST(GraphicsCompositionPixel, NativeFadeAndReversalReachActualGpuPixels) {
  auto compositor = corevideo::modules::createD3D11Compositor();
  ASSERT_NE(compositor, nullptr);
  using corevideo::rpc::Json;
  double now = 0;
  corevideo::graphics::GraphicsInstances graphics{"test-core", [&] { return now; }};
  int sequence = 0;
  const auto command = [&](const std::string& type, Json::Object payload) {
    payload["schemaVersion"] = 1; payload["requestId"] = "operation-" + std::to_string(++sequence);
    payload["clientId"] = "timeline-gpu-qa"; payload["coreEpoch"] = "test-core";
    payload["instanceId"] = "featured"; payload["expectedRevision"] = 1;
    return graphics.apply(Json::Object{{"type", type}, {"graphics", std::move(payload)}});
  };
  const auto render = [&] {
    corevideo::modules::CompositorRenderPlan plan;
    plan.width = 320; plan.height = 180;
    graphics.append(plan, false);
    auto frame = compositor->render(plan, {});
    graphics.observe(frame);
    return frame.preview.bgra;
  };
  const auto blank = render();
  ASSERT_FALSE(blank.empty());
  ASSERT_TRUE(command("graphics-instance-create", {{"templateId", "compact-solid"}, {"templateVersion", 1},
      {"scope", "global"}, {"fields", Json::Object{{"name", "Native animation"}}},
      {"animations", Json::Object{
          {"in", Json::Object{{"type", "fade"}, {"durationMs", 1000}}},
          {"out", Json::Object{{"type", "fade"}, {"durationMs", 1000}}}}}}));
  ASSERT_TRUE(command("graphics-instance-trigger", {{"action", "show"}}));
  const auto row = [&]() -> const Json& { return graphics.snapshot().get("instances")->asArray().front(); };
  const auto limit = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (row().getString("preparation") != "ready" && std::chrono::steady_clock::now() < limit) {
    render(); std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  ASSERT_EQ("ready", row().getString("preparation"));
  EXPECT_EQ(blank, render()); // zero-opacity native entrance
  now = 500;
  const auto half = render();
  EXPECT_NE(blank, half);
  now = 1000;
  const auto full = render();
  EXPECT_NE(half, full);
  ASSERT_TRUE(command("graphics-instance-trigger", {{"action", "hide"}}));
  EXPECT_EQ(full, render());
  now = 1500;
  EXPECT_EQ(half, render());
  ASSERT_TRUE(command("graphics-instance-trigger", {{"action", "show"}}));
  EXPECT_EQ(half, render()); // exact native pixels across the reversal boundary
  EXPECT_EQ(1., row().getNumber("revision"));
  ASSERT_TRUE(command("graphics-program-clear", {{"channel", "all"}}));
  EXPECT_EQ(blank, render());
}

TEST(GraphicsCompositionPixel, AutomaticIntroductionUsesActualSourceDrawAndClearPreventsReplay) {
  using corevideo::rpc::Json;
  auto compositor = corevideo::modules::createD3D11Compositor();
  ASSERT_NE(compositor, nullptr);
  double now=0;corevideo::graphics::GraphicsInstances graphics{"test-core",[&]{return now;}};
  corevideo::modules::ZoomEngineParticipant participant;
  participant.id = 44; participant.sourceGeneration = 1; participant.displayName = "First guest"; participant.hasVideo = true;
  corevideo::modules::VideoFrame video;
  video.participantId = "44"; video.sourceGeneration = 1; video.sourceEpoch = 1; video.frameId = 1;
  video.width = video.pixelWidth = video.height = video.pixelHeight = 1; video.pixelStride = 4;
  video.pixels = std::make_shared<const std::vector<uint8_t>>(std::vector<uint8_t>{20, 30, 170, 255});
  graphics.observeZoomSources({participant}, 7, {video});
  const auto binding = [&] { return *graphics.snapshot().get("sources")->asArray().front().get("identity"); };
  int sequence = 0;
  const auto command = [&](const std::string& type, Json::Object payload) {
    payload["schemaVersion"] = 1; payload["requestId"] = "operation-" + std::to_string(++sequence);
    payload["clientId"] = "binding-gpu-qa"; payload["coreEpoch"] = "test-core";
    payload["instanceId"] = "featured";
    if (!payload.count("expectedRevision")) payload["expectedRevision"] = 1;
    return graphics.apply(Json::Object{{"type", type}, {"graphics", std::move(payload)}});
  };
  const auto basePlan = [&] {
    corevideo::modules::CompositorRenderPlan plan;
    plan.width = 320; plan.height = 180;
    corevideo::modules::CompositorRenderPlanLayer source;
    source.sourceId = "zoom:44"; source.participantId = "44"; source.kind = "participant-video";
    plan.layers.push_back(source);
    return plan;
  };
  const auto render = [&] {
    auto plan = basePlan(); graphics.append(plan, false, {video});
    auto frame = compositor->render(plan, {video}); graphics.observe(frame);
    return frame.preview.bgra;
  };
  const auto row=[&]() -> const Json& {return graphics.snapshot().get("instances")->asArray().front();};
  const auto clean=compositor->render(basePlan(),{video}).preview.bgra;
  ASSERT_TRUE(command("graphics-instance-create",{{"templateId","compact-solid"},{"templateVersion",1},{"scope","global"},{"fields",Json::Object{{"name","Private placeholder"}}}}));
  Json config=Json::Object{{"rules",Json::Array{Json::Object{{"id","intro"},{"instanceId","featured"},{"dwellMs",100},{"durationMs",0}}}}};
  ASSERT_TRUE(command("graphics-automation-configure",{{"configuration",config},{"expectedAutomationRevision",1}}));
  ASSERT_TRUE(command("graphics-automation-control",{{"action","set-mode"},{"mode","automatic"}}));
  ASSERT_TRUE(command("graphics-automation-control",{{"action","arm"},{"mode","automatic"},{"expectedAutomationRevision",2},{"expectedActivationRevision",graphics.snapshot().get("automation")->getNumber("activationRevision")}}));
  render();EXPECT_EQ(row().getNumber("programRevision"),0);now=101;render();
  const auto until=std::chrono::steady_clock::now()+std::chrono::seconds(5);
  while(row().getNumber("firstDrawSubmittedFrame")==0 && std::chrono::steady_clock::now()<until) {render();std::this_thread::sleep_for(std::chrono::milliseconds(2));}
  ASSERT_GT(row().getNumber("firstDrawSubmittedFrame"),0);EXPECT_NE(clean,render());
  EXPECT_EQ(row().get("programFields")->getString("name"),"First guest");
  const auto& trace=graphics.snapshot().get("automation")->get("trace")->asArray();
  EXPECT_TRUE(std::any_of(trace.begin(),trace.end(),[](const auto& item){return item.getString("state")=="draw-submitted";}));
  ASSERT_TRUE(command("graphics-program-clear",{{"channel","all"}}));now=2000;
  EXPECT_EQ(clean,render());now=4000;EXPECT_EQ(clean,render());
  EXPECT_TRUE(graphics.snapshot().get("automation")->get("inhibited")->asBool());
}

TEST(GraphicsCompositionPixel, SourceGenerationReplacementCannotBurnThePreviousGuestIntoNewVideo) {
  using corevideo::rpc::Json;
  auto compositor = corevideo::modules::createD3D11Compositor();
  ASSERT_NE(compositor, nullptr);
  corevideo::graphics::GraphicsInstances graphics{"test-core"};
  corevideo::modules::ZoomEngineParticipant participant;
  participant.id = 44; participant.sourceGeneration = 1; participant.displayName = "First guest"; participant.hasVideo = true;
  corevideo::modules::VideoFrame video;
  video.participantId = "44"; video.sourceGeneration = 1; video.sourceEpoch = 1; video.frameId = 1;
  video.width = video.pixelWidth = video.height = video.pixelHeight = 1; video.pixelStride = 4;
  video.pixels = std::make_shared<const std::vector<uint8_t>>(std::vector<uint8_t>{20, 30, 170, 255});
  graphics.observeZoomSources({participant}, 7, {video});
  const auto binding = [&] { return *graphics.snapshot().get("sources")->asArray().front().get("identity"); };
  int sequence = 0;
  const auto command = [&](const std::string& type, Json::Object payload) {
    payload["schemaVersion"] = 1; payload["requestId"] = "operation-" + std::to_string(++sequence);
    payload["clientId"] = "binding-gpu-qa"; payload["coreEpoch"] = "test-core";
    payload["instanceId"] = "featured";
    if (!payload.count("expectedRevision")) payload["expectedRevision"] = 1;
    return graphics.apply(Json::Object{{"type", type}, {"graphics", std::move(payload)}});
  };
  const auto basePlan = [&] {
    corevideo::modules::CompositorRenderPlan plan;
    plan.width = 320; plan.height = 180;
    corevideo::modules::CompositorRenderPlanLayer source;
    source.sourceId = "zoom:44"; source.participantId = "44"; source.kind = "participant-video";
    plan.layers.push_back(source);
    return plan;
  };
  const auto render = [&] {
    auto plan = basePlan(); graphics.append(plan, false, {video});
    auto frame = compositor->render(plan, {video}); graphics.observe(frame);
    return frame.preview.bgra;
  };
  const auto row = [&]() -> const Json& { return graphics.snapshot().get("instances")->asArray().front(); };
  const auto pumpUntilDrawn = [&] {
    const auto limit = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (row().getNumber("firstDrawSubmittedFrame") == 0 && std::chrono::steady_clock::now() < limit) {
      render(); std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return row().getNumber("firstDrawSubmittedFrame") > 0;
  };
  ASSERT_TRUE(command("graphics-instance-create", {{"templateId", "compact-solid"}, {"templateVersion", 1},
      {"scope", "global"}, {"binding", binding()}, {"fields", Json::Object{{"name", "First guest"}, {"title", ""}}}}));
  ASSERT_TRUE(command("graphics-instance-trigger", {{"action", "show"}}));
  ASSERT_TRUE(pumpUntilDrawn());
  const auto first = render(); ASSERT_FALSE(first.empty());
  auto qualifiedPlan = basePlan(); graphics.append(qualifiedPlan, false, {video});
  ASSERT_EQ(2u, qualifiedPlan.layers.size());
  auto metadataOnly = video; metadataOnly.pixels.reset();
  const auto slate = compositor->render(basePlan(), {metadataOnly}).preview.bgra;
  EXPECT_EQ(slate, compositor->render(qualifiedPlan, {metadataOnly}).preview.bgra);
  EXPECT_EQ(first, render()); // no source draw from a prior pass can qualify this pass
  video.sourceEpoch = UINT64_MAX; // format/texture epoch is not the subject identity
  graphics.observeZoomSources({participant}, 7, {video});
  EXPECT_EQ(first, render());
  participant.sourceGeneration = 2; participant.displayName = "Second guest";
  video.sourceGeneration = 2; video.sourceEpoch = 9; video.frameId = 2;
  video.pixels = std::make_shared<const std::vector<uint8_t>>(std::vector<uint8_t>{40, 170, 30, 255});
  graphics.observeZoomSources({participant}, 7, {video});
  const auto cleanReplacement = compositor->render(basePlan(), {video}).preview.bgra;
  EXPECT_EQ(cleanReplacement, render()); // no old subject text over the replacement pixels
  EXPECT_EQ("hidden", row().getString("visibility"));
  EXPECT_FALSE(command("graphics-instance-trigger", {{"action", "show"}}));
  ASSERT_TRUE(command("graphics-instance-update", {{"target", "cue"}, {"binding", binding()},
      {"fields", Json::Object{{"name", "Second guest"}, {"title", ""}}}}));
  ASSERT_TRUE(command("graphics-instance-trigger", {{"action", "show"}, {"expectedRevision", 2}}));
  ASSERT_TRUE(pumpUntilDrawn());
  EXPECT_NE(cleanReplacement, render());
  EXPECT_EQ("Second guest", row().get("programFields")->getString("name"));
  EXPECT_EQ("", row().get("programFields")->getString("title"));
}

TEST(GraphicsCompositionPixel, FailedFontPreparationRetainsAcceptedPixelsAndRepairsWithoutRestart) {
  using corevideo::rpc::Json;
  auto compositor = corevideo::modules::createD3D11Compositor(); ASSERT_NE(compositor, nullptr);
  corevideo::graphics::GraphicsInstances graphics{"test-core"};
  int sequence = 0;
  const auto command = [&](const std::string& type, Json::Object payload) {
    payload["schemaVersion"] = 1; payload["requestId"] = "operation-" + std::to_string(++sequence);
    payload["clientId"] = "font-gpu-qa"; payload["coreEpoch"] = "test-core"; payload["instanceId"] = "featured";
    if (!payload.count("expectedRevision")) payload["expectedRevision"] = 1;
    return graphics.apply(Json::Object{{"type", type}, {"graphics", std::move(payload)}});
  };
  const auto row = [&]() -> const Json& { return graphics.snapshot().get("instances")->asArray().front(); };
  const auto render = [&] {
    corevideo::modules::CompositorRenderPlan plan; plan.width = 320; plan.height = 180;
    graphics.append(plan, false);
    auto frame = compositor->render(plan, {}); graphics.observe(frame);
    return frame.preview.bgra;
  };
  const auto until = [&](const auto& predicate) {
    const auto limit = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!predicate() && std::chrono::steady_clock::now() < limit) {
      render(); std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return predicate();
  };
  ASSERT_TRUE(command("graphics-instance-create", {{"templateId", "compact-solid"}, {"templateVersion", 1},
      {"scope", "global"}, {"fields", Json::Object{{"name", "Accepted guest"}}}}));
  ASSERT_TRUE(command("graphics-instance-trigger", {{"action", "show"}}));
  ASSERT_TRUE(until([&] { return row().getNumber("firstDrawSubmittedFrame") > 0; }));
  const auto accepted = render(); ASSERT_FALSE(accepted.empty());
  ASSERT_TRUE(command("graphics-instance-update", {{"target", "program"},
      {"style", Json::Object{{"fontFamily", "CoreVideo Missing Font 9c2a8f77"}}}}));
  ASSERT_TRUE(until([&] { return row().getString("preparation") == "failed"; }));
  EXPECT_EQ(1., row().getNumber("programRevision"));
  EXPECT_EQ(accepted, render());
  EXPECT_FALSE(row().get("showPending")->asBool());
  EXPECT_FALSE(command("graphics-instance-trigger", {{"action", "show"}, {"expectedRevision", 2}}));
  ASSERT_TRUE(command("graphics-instance-update", {{"target", "program"}, {"expectedRevision", 2},
      {"style", Json::Object{{"fontFamily", "Segoe UI"}}}, {"fields", Json::Object{{"name", "Repaired guest"}}}}));
  ASSERT_TRUE(until([&] { return row().getNumber("programRevision") == 3 && row().getNumber("firstDrawSubmittedFrame") > 0; }));
  EXPECT_NE(accepted, render());
  EXPECT_TRUE(row().get("programResource")->isObject());
}

TEST(GraphicsCompositionPixel, AcceptedRasterSurvivesDraftCachePressureAndANewRendererWithoutAnotherShow) {
  using corevideo::rpc::Json;
  auto compositor = corevideo::modules::createD3D11Compositor(); ASSERT_NE(compositor, nullptr);
  corevideo::graphics::GraphicsInstances graphics{"test-core"};
  int sequence = 0;
  const auto command = [&](const std::string& type, Json::Object payload) {
    payload["schemaVersion"] = 1; payload["requestId"] = "operation-" + std::to_string(++sequence);
    payload["clientId"] = "residency-gpu-qa"; payload["coreEpoch"] = "test-core"; payload["instanceId"] = "featured";
    return graphics.apply(Json::Object{{"type", type}, {"graphics", std::move(payload)}});
  };
  const auto row = [&]() -> const Json& { return graphics.snapshot().get("instances")->asArray().front(); };
  const auto render = [&](corevideo::modules::ICompositor& renderer) {
    corevideo::modules::CompositorRenderPlan plan; plan.width = 320; plan.height = 180;
    graphics.append(plan, false);
    auto frame = renderer.render(plan, {}); graphics.observe(frame);
    return frame.preview.bgra;
  };
  const auto until = [&](const auto& predicate, corevideo::modules::ICompositor& renderer) {
    const auto limit = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!predicate() && std::chrono::steady_clock::now() < limit) {
      render(renderer); std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return predicate();
  };
  ASSERT_TRUE(command("graphics-instance-create", {{"templateId", "compact-solid"}, {"templateVersion", 1},
      {"scope", "global"}, {"fields", Json::Object{{"name", "Accepted Program"}}}}));
  ASSERT_TRUE(command("graphics-instance-cue", {{"expectedRevision", 1}}));
  ASSERT_TRUE(command("graphics-instance-trigger", {{"expectedRevision", 1}, {"action", "show"}}));
  ASSERT_TRUE(until([&] { return row().getNumber("firstDrawSubmittedFrame") > 0; }, *compositor));
  const auto accepted = render(*compositor); ASSERT_FALSE(accepted.empty());
  const auto acceptedKey = row().get("programResource")->getString("contentKey");
  ASSERT_EQ(64u, acceptedKey.size());
  // More sequential private revisions than the 16-entry cache; obsolete, unpinned drafts retire.
  constexpr int latestRevision = 80;
  for (int revision = 2; revision <= latestRevision; ++revision) {
    ASSERT_TRUE(command("graphics-instance-update", {{"expectedRevision", revision - 1}, {"target", "cue"},
        {"fields", Json::Object{{"name", "Private revision " + std::to_string(revision)}}}}));
    ASSERT_TRUE(until([&] { return row().getString("preparation") == "ready"; }, *compositor));
    EXPECT_EQ(accepted, render(*compositor));
    EXPECT_EQ(acceptedKey, row().get("programResource")->getString("contentKey"));
  }
  corevideo::modules::CompositorRenderPlan original; original.width = 320; original.height = 180;
  graphics.append(original, false);
  ASSERT_EQ(1u, original.layers.size());
  const auto oldDevice = original.layers.front().graphicsPreparedResource->deviceIdentity;
  auto replacement = corevideo::modules::createD3D11Compositor(); ASSERT_NE(replacement, nullptr);
  bool restored = false;
  ASSERT_TRUE(until([&] { restored = render(*replacement) == accepted; return restored; }, *replacement));
  corevideo::modules::CompositorRenderPlan recovered; recovered.width = 320; recovered.height = 180;
  graphics.append(recovered, false);
  ASSERT_EQ(1u, recovered.layers.size());
  EXPECT_NE(oldDevice, recovered.layers.front().graphicsPreparedResource->deviceIdentity);
  EXPECT_EQ(1., row().getNumber("programRevision"));
  EXPECT_EQ(double(latestRevision), row().getNumber("revision"));
  EXPECT_EQ("Accepted Program", row().get("programFields")->getString("name"));
  EXPECT_EQ(acceptedKey, row().get("programResource")->getString("contentKey"));
  // This proves eventual pixel recovery only, not continuity during device replacement.
}

TEST(GraphicsCompositionPixel, AcceptedCuePixelsStaySeparateFromPrivateEditsAndProgramUntilExplicitShow) {
  using corevideo::rpc::Json;
  auto compositor = corevideo::modules::createD3D11Compositor(); ASSERT_NE(compositor, nullptr);
  corevideo::graphics::GraphicsInstances graphics{"test-core"};
  int sequence = 0;
  const auto command = [&](const std::string& type, Json::Object payload) {
    payload["schemaVersion"] = 1; payload["requestId"] = "operation-" + std::to_string(++sequence);
    payload["clientId"] = "cue-gpu-qa"; payload["coreEpoch"] = "test-core"; payload["instanceId"] = "featured";
    return graphics.apply(Json::Object{{"type", type}, {"graphics", std::move(payload)}});
  };
  const auto row = [&]() -> const Json& { return graphics.snapshot().get("instances")->asArray().front(); };
  const auto pixels = [&](bool cue) {
    corevideo::modules::CompositorRenderPlan plan; plan.width = 320; plan.height = 180;
    graphics.append(plan, cue);
    auto frame = compositor->render(plan, {});
    if (!cue) graphics.observe(frame);
    return frame.preview.bgra;
  };
  const auto untilReady = [&] {
    const auto limit = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (row().getString("preparation") != "ready" && std::chrono::steady_clock::now() < limit) {
      pixels(false); std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return row().getString("preparation") == "ready";
  };
  const auto blank = pixels(false); ASSERT_FALSE(blank.empty());
  ASSERT_TRUE(command("graphics-instance-create", {{"templateId", "compact-solid"}, {"templateVersion", 1},
      {"scope", "global"}, {"fields", Json::Object{{"name", "Accepted Cue"}, {"title", "Presenter"}}}}));
  ASSERT_TRUE(command("graphics-instance-cue", {{"expectedRevision", 1}})); ASSERT_TRUE(untilReady());
  const auto firstCue = pixels(true); EXPECT_NE(blank, firstCue); EXPECT_EQ(blank, pixels(false));
  ASSERT_TRUE(command("graphics-instance-update", {{"expectedRevision", 1}, {"target", "cue"},
      {"fields", Json::Object{{"name", "Private edit"}, {"title", ""}}}}));
  ASSERT_TRUE(untilReady());
  EXPECT_EQ(firstCue, pixels(true)); EXPECT_EQ(blank, pixels(false));
  ASSERT_TRUE(command("graphics-instance-trigger", {{"expectedRevision", 1}, {"target", "cue"}, {"action", "show"}}));
  EXPECT_EQ(firstCue, pixels(false));
  ASSERT_TRUE(command("graphics-instance-cue", {{"expectedRevision", 2}}));
  const auto secondCue = pixels(true); EXPECT_NE(firstCue, secondCue); EXPECT_EQ(firstCue, pixels(false));
  ASSERT_TRUE(command("graphics-instance-trigger", {{"expectedRevision", 2}, {"target", "cue"}, {"action", "show"}}));
  EXPECT_EQ(secondCue, pixels(false));
}

TEST(GraphicsCompositionPixel, EightProgramAndCueInstancesRefuseExcessRevisionsWithoutLosingAcceptedPixels) {
  using corevideo::rpc::Json;
  auto compositor = corevideo::modules::createD3D11Compositor(); ASSERT_NE(compositor, nullptr);
  corevideo::graphics::GraphicsInstances graphics{"test-core"};
  int operation = 0, revisionClock = 0;
  std::array<int, 8> revisions{};
  const auto id = [](int index) { return "graphic-" + std::to_string(index + 1); };
  const auto command = [&](const std::string& type, int index, Json::Object payload) {
    payload["schemaVersion"] = 1; payload["requestId"] = "operation-" + std::to_string(++operation);
    payload["clientId"] = "eight-instance-gpu-qa"; payload["coreEpoch"] = "test-core"; payload["instanceId"] = id(index);
    if (type != "graphics-instance-create") payload["expectedRevision"] = revisions[index];
    return graphics.apply(Json::Object{{"type", type}, {"graphics", std::move(payload)}});
  };
  const auto rows = [&]() -> const Json::Array& { return graphics.snapshot().get("instances")->asArray(); };
  const auto render = [&](bool cue = false) {
    corevideo::modules::CompositorRenderPlan plan; plan.width = 320; plan.height = 180;
    graphics.append(plan, cue);
    auto frame = compositor->render(plan, {});
    if (!cue) graphics.observe(frame);
    return frame.preview.bgra;
  };
  const auto until = [&](const auto& predicate) {
    const auto limit = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (!predicate() && std::chrono::steady_clock::now() < limit) {
      render(); std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return predicate();
  };
  for (int index = 0; index < 8; ++index) {
    ASSERT_TRUE(command("graphics-instance-create", index, {{"templateId", "compact-solid"}, {"templateVersion", 1},
        {"scope", "global"}, {"fields", Json::Object{{"name", "Program " + std::to_string(index + 1)}}}}));
    revisions[index] = ++revisionClock;
    ASSERT_TRUE(command("graphics-instance-trigger", index, {{"action", "show"}}));
  }
  ASSERT_TRUE(until([&] { return std::all_of(rows().begin(), rows().end(), [](const auto& row) {
    return row.getNumber("firstDrawSubmittedFrame") > 0;
  }); }));
  const auto program = render(); ASSERT_FALSE(program.empty());
  for (int index = 0; index < 8; ++index) {
    ASSERT_TRUE(command("graphics-instance-update", index, {{"target", "cue"},
        {"fields", Json::Object{{"name", "Cue " + std::to_string(index + 1)}}}}));
    revisions[index] = ++revisionClock;
    ASSERT_TRUE(command("graphics-instance-cue", index, {}));
  }
  ASSERT_TRUE(until([&] { return std::all_of(rows().begin(), rows().end(), [](const auto& row) {
    return row.getNumber("cueRevision") == row.getNumber("revision");
  }); }));
  const auto cue = render(true); EXPECT_NE(program, cue);
  for (int index = 0; index < 8; ++index) {
    EXPECT_FALSE(command("graphics-instance-update", index, {{"target", "cue"},{"fields",Json::Object{{"name","Private excess"}}}}));
    EXPECT_EQ(graphics.snapshot().get("lastResult")->getString("reason"),"graphics-resident-revision-capacity");
  }
  ASSERT_EQ(8u,rows().size());
  for(size_t index=0;index<rows().size();++index) {
    const auto& row=rows()[index];EXPECT_EQ(double(index+1),row.getNumber("programRevision"));EXPECT_EQ(double(index+9),row.getNumber("cueRevision"));
    EXPECT_EQ(row.getNumber("revision"),row.getNumber("cueRevision"));EXPECT_TRUE(row.get("programResource")->isObject());EXPECT_TRUE(row.get("cueResource")->isObject());
  }
  EXPECT_EQ(program, render()); EXPECT_EQ(cue, render(true));
  const auto budget = corevideo::graphics::GraphicsResourceBudget::process().counters();
  EXPECT_LE(budget.gpuBytes, budget.gpuLimit); EXPECT_LE(budget.cpuHighWater, budget.cpuLimit);
  EXPECT_LE(budget.preparationHighWater, 2u);
  // Offscreen 320x180 composition/identity coverage, not full-output delivery acceptance.
}

#endif
