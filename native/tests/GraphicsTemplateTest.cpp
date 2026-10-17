#include "graphics/GraphicsTemplate.h"
#include "graphics/GraphicsTemplateRegistry.h"
#include "graphics/GraphicsTemplateDraft.h"
#include "modules/Sha256.h"
#include "gtest/gtest.h"
#include "GraphicsAssetFixture.h"
#include <chrono>
#include <thread>
#include <fstream>
#include <cstdlib>

using corevideo::graphics::GraphicsTemplate;
using corevideo::graphics::GraphicsDrawing;
using corevideo::rpc::Json;
namespace {
Json::Object manifest() { return GraphicsTemplate::corporateLowerThird()->document().asObject(); }
Json brand() { return Json::Object{{"brand.primary", "#10203080"}, {"brand.onPrimary", "#FFFFFF"},
    {"brand.onPrimaryMuted", "#CCDDFF"}, {"brand.accent", "#44C1A1"}, {"brand.fontBody", "Segoe UI"}}; }
std::shared_ptr<const GraphicsTemplate> load(Json::Object object, std::string& error) {
  return GraphicsTemplate::load(Json(std::move(object)).stringify(), error);
}

TEST(GraphicsTemplateRegistry, WorkerInstallsImmutableVersionsAndRefusesReservedIdsAndMalformedDocuments) {
  corevideo::graphics::GraphicsTemplateRegistry registry;
  const auto complete = [&](const std::string& request) {
    std::string state;
    const auto limit = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (std::chrono::steady_clock::now() < limit) {
      const auto publication = registry.poll();
      for (const auto& row : publication->installs.asArray()) if (row.getString("requestId") == request) {
        state = row.getString("state");
        if (state == "installed" || state == "failed") return row.getString("reason");
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return std::string("timed-out-") + state;
  };
  const auto submit = [&](const std::string& request, const std::string& document) {
    std::string result;
    const auto limit = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    do {
      result = registry.submit("qa", request, document);
      if (result != "template-import-busy") return result;
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    } while (std::chrono::steady_clock::now() < limit);
    return result;
  };
  auto doc = manifest(); doc["templateId"] = "imported-interview"; doc["templateVersion"] = 2;
  const auto bytes = Json(doc).stringify();
  EXPECT_EQ("template-install-queued", submit("first", bytes));
  EXPECT_EQ("", complete("first"));
  auto first = registry.poll()->definitions.at({"imported-interview", 2});
  EXPECT_EQ("template-install-queued", submit("first", bytes));
  EXPECT_EQ("request-id-reused", submit("first", "{}"));
  doc["name"] = "Altered same version";
  EXPECT_EQ("template-install-queued", submit("collision", Json(doc).stringify()));
  EXPECT_EQ("template-version-immutable", complete("collision"));
  EXPECT_EQ(first, registry.poll()->definitions.at({"imported-interview", 2}));
  doc["templateVersion"] = 3;
  EXPECT_EQ("template-install-queued", submit("next", Json(doc).stringify())); EXPECT_EQ("", complete("next"));
  EXPECT_EQ(first, registry.poll()->definitions.at({"imported-interview", 2}));
  doc["templateId"] = "broadcast";
  EXPECT_EQ("template-install-queued", submit("reserved", Json(doc).stringify()));
  EXPECT_EQ("template-id-reserved", complete("reserved"));
  EXPECT_EQ("template-install-queued", submit("invalid", "{\"schemaVersion\":2}"));
  EXPECT_FALSE(complete("invalid").empty());
  EXPECT_EQ("template-manifest-size", submit("huge", std::string(256 * 1024 + 1, 'x')));
}
}

TEST(GraphicsTemplate, BuiltinResolvesTypedFieldsBrandAlphaAndCollapsedOptionalText) {
  const auto definition = GraphicsTemplate::corporateLowerThird(); ASSERT_NE(definition, nullptr);
  std::string error; GraphicsDrawing drawing;
  EXPECT_FALSE(definition->resolve(definition->defaultFields(), brand(), drawing, error));
  EXPECT_EQ("required-template-field-empty", error);
  const Json fields = Json::Object{{"name", "Presenter — العربية 日本語"}, {"title", ""}};
  ASSERT_TRUE(definition->resolve(fields, brand(), drawing, error));
  EXPECT_EQ(1050., drawing.width); EXPECT_EQ(176., drawing.height);
  ASSERT_EQ(2u, drawing.primitives.size());
  EXPECT_EQ(0x80102030u, drawing.primitives[0].color);
  EXPECT_EQ("Presenter — العربية 日本語", drawing.primitives[1].text);
  EXPECT_EQ(24., drawing.primitives[1].box.x); EXPECT_EQ(20., drawing.primitives[1].box.y);
  EXPECT_EQ(600, drawing.primitives[1].fontWeight);
  EXPECT_EQ("Segoe UI", drawing.primitives[1].font);
}

TEST(GraphicsTemplate, OrderedFlowAndExplicitClippingAreResolvedInReferenceUnits) {
  const auto definition = GraphicsTemplate::corporateLowerThird(); std::string error; GraphicsDrawing drawing;
  ASSERT_TRUE(definition->resolve(Json::Object{{"name", "Guest"}, {"title", "Engineer"}}, brand(), drawing, error));
  ASSERT_EQ(3u, drawing.primitives.size());
  EXPECT_EQ(90., drawing.primitives[2].box.y); // 20 + 62 + 8
  EXPECT_EQ(1002., drawing.primitives[2].clip.width);
  EXPECT_EQ(42., drawing.primitives[2].clip.height);
}

TEST(GraphicsTemplate, ExactNumbersAndImmutableHashesSurviveNativeExportAndReimport) {
  EXPECT_EQ("100000", Json(100000).stringify());
  EXPECT_EQ("9007199254740991", Json(9007199254740991.).stringify());
  for (const auto number : {1.2345678901234567, 1e-5, 1e90, 1e-90, 1e-320, -0.12345678901234567}) {
    const auto text = Json(number).stringify(); const auto parsed = Json::parse(text);
    ASSERT_TRUE(parsed.has_value()); EXPECT_EQ(number, parsed->asNumber());
  }
  std::string error;
  auto original = manifest(); auto layers = original["layers"].asArray(); auto plate = layers[0].asObject();
  plate["cornerRadius"] = 8.12345678901234; layers[0] = plate; original["layers"] = layers;
  auto first = load(original, error); ASSERT_TRUE(first);
  auto exported = GraphicsTemplate::load(first->document().stringify(), error); ASSERT_TRUE(exported);
  EXPECT_EQ(first->hash(), exported->hash());
  plate["cornerRadius"] = 8.12345678901235; layers[0] = plate; original["layers"] = layers;
  auto second = load(original, error); ASSERT_TRUE(second); EXPECT_NE(first->hash(), second->hash());
}

TEST(GraphicsTemplate, MalformedDuplicateDeepAndOversizedDocumentsAreRejectedBeforeAdoption) {
  std::string error;
  for (const auto& input : {std::string(256 * 1024 + 1, ' '), std::string("{\"schemaVersion\":1,\"schemaVersion\":2}"),
      std::string(40, '[') + "0" + std::string(40, ']'), std::string("{\"x\":01}"), std::string("{\"x\":1e999}"),
      std::string("{\"x\":\"\\ud800\"}"), std::string("{\"x\":\"\\udc00\"}"), std::string("{\"x\":\"unterminated}"),
      std::string("{\"x\":NaN}"), std::string("{\"x\":true,}")}) {
    EXPECT_FALSE(GraphicsTemplate::load(input, error)); EXPECT_FALSE(error.empty());
  }
}

TEST(GraphicsTemplate, VersionUnknownNodesFieldsAndExecutablePropertiesAreNotSilentlyIgnored) {
  std::string error;
  auto object = manifest(); object["schemaVersion"] = 2; EXPECT_FALSE(load(object, error));
  object = manifest(); object["script"] = "alert(1)"; EXPECT_FALSE(load(object, error));
  object = manifest(); auto layers = object["layers"].asArray(); auto first = layers[0].asObject();
  first["type"] = "html"; layers[0] = first; object["layers"] = layers; EXPECT_FALSE(load(object, error));
  object = manifest(); auto fields = object["fields"].asArray(); auto field = fields[0].asObject();
  field["type"] = "expression"; fields[0] = field; object["fields"] = fields; EXPECT_FALSE(load(object, error));
  object = manifest(); object["templateVersion"] = 1.5; EXPECT_FALSE(load(object, error));
}

TEST(GraphicsTemplate, UnknownBrandOrFieldReferencesAndWrongOptionalTypesAreRefused) {
  std::string error;
  auto object = manifest(); auto layers = object["layers"].asArray(); auto group = layers[1].asObject();
  group["flow"] = 1; layers[1] = group; object["layers"] = layers; EXPECT_FALSE(load(object, error));
  object = manifest(); layers = object["layers"].asArray(); auto plate = layers[0].asObject();
  plate["fill"] = Json::Object{{"token", "brand.secret"}}; layers[0] = plate; object["layers"] = layers; EXPECT_FALSE(load(object, error));
  auto definition = GraphicsTemplate::corporateLowerThird();
  EXPECT_FALSE(definition->validateFields(Json::Object{{"name", true}, {"title", ""}}, false, error));
  EXPECT_FALSE(definition->validateFields(Json::Object{{"name", "Guest"}, {"title", ""}, {"script", "no"}}, false, error));
  GraphicsDrawing drawing;
  EXPECT_FALSE(definition->resolve(Json::Object{{"name", "Guest"}, {"title", ""}}, Json::Object{}, drawing, error));
}

TEST(GraphicsTemplate, MetadataRoundTripsAndChangesContentIdentityWithoutMutableVersions) {
  auto object = manifest(); object["metadata"] = Json::Object{{"publisher", "Example"}, {"future", Json::Array{1, 2, 3}}};
  std::string error; auto definition = load(object, error); ASSERT_NE(definition, nullptr);
  EXPECT_EQ("Example", definition->document().get("metadata")->getString("publisher"));
  EXPECT_NE(GraphicsTemplate::corporateLowerThird()->hash(), definition->hash());
  const auto again = GraphicsTemplate::load(definition->document().stringify(), error); ASSERT_NE(again, nullptr);
  EXPECT_EQ(definition->hash(), again->hash());
}

TEST(GraphicsAssets, PackageRootHashesHeadersAndImmutableBytesAreValidatedBeforeRendering) {
  std::string error;
  const auto asset = corevideo::graphics::graphicsAssetFromBytes("logo", graphicsTest::png(), error); ASSERT_TRUE(asset);
  EXPECT_EQ(2, asset->width); EXPECT_EQ(1, asset->height);
  const auto document = graphicsTest::imageDocument(asset);
  corevideo::graphics::GraphicsAssets assets;
  EXPECT_FALSE(corevideo::graphics::loadGraphicsAssets(document, "", assets, error));
  graphicsTest::Package package(asset);
  ASSERT_TRUE(corevideo::graphics::loadGraphicsAssets(document, package.root.string(), assets, error));
  ASSERT_EQ(1u, assets.size()); EXPECT_EQ(asset->sha256, assets.at("logo")->sha256);
  auto definition = GraphicsTemplate::load(document.stringify(), error, assets); ASSERT_TRUE(definition);
  GraphicsDrawing drawing; ASSERT_TRUE(definition->resolve(Json::Object{}, Json::Object{}, drawing, error));
  ASSERT_EQ(1u, drawing.primitives.size()); ASSERT_TRUE(drawing.primitives[0].image);
  EXPECT_EQ(*asset->encoded, *drawing.primitives[0].image->encoded);
  { std::ofstream changed(package.file, std::ios::binary | std::ios::trunc); changed << "not an image"; }
  EXPECT_FALSE(corevideo::graphics::loadGraphicsAssets(document, package.root.string(), assets, error));
  EXPECT_FALSE(error.empty());
  EXPECT_EQ(*asset->encoded, *definition->assets().at("logo")->encoded); // accepted bytes cannot change under a raster
  auto bad = document.asObject(); auto declarations = bad["assets"].asArray(); auto first = declarations[0].asObject();
  first["file"] = "../outside.png"; declarations[0] = first; bad["assets"] = declarations;
  EXPECT_FALSE(GraphicsTemplate::load(Json(bad).stringify(), error));
}

TEST(GraphicsAssets, OptionalImageHasExplicitFallbackAndWarningWhileRequiredImageBlocksReadiness) {
  std::string error;
  const auto asset = corevideo::graphics::graphicsAssetFromBytes("logo", graphicsTest::png(), error); ASSERT_TRUE(asset);
  auto document = graphicsTest::imageDocument(asset).asObject();
  auto definition = GraphicsTemplate::load(Json(document).stringify(), error); ASSERT_TRUE(definition);
  GraphicsDrawing drawing; EXPECT_FALSE(definition->resolve(Json::Object{}, Json::Object{}, drawing, error));
  auto declarations = document["assets"].asArray(); auto declaration = declarations[0].asObject(); declaration["optional"] = true;
  declarations[0] = declaration; document["assets"] = declarations;
  auto layers = document["layers"].asArray(); auto node = layers[0].asObject(); node["missing"] = "hide"; layers[0] = node; document["layers"] = layers;
  definition = GraphicsTemplate::load(Json(document).stringify(), error); ASSERT_TRUE(definition);
  ASSERT_TRUE(definition->resolve(Json::Object{}, Json::Object{}, drawing, error));
  EXPECT_TRUE(drawing.primitives.empty()); ASSERT_EQ(1u, drawing.warnings.size());
  EXPECT_NE(std::string::npos, drawing.warnings[0].find("logo"));
}

TEST(GraphicsTemplateRegistry, EncodedPackagePoolSharesVersionsAndFailedImportCannotReplaceAcceptedBytes) {
  std::string error;
  const auto asset = corevideo::graphics::graphicsAssetFromBytes("logo", graphicsTest::png(), error); ASSERT_TRUE(asset);
  graphicsTest::Package package(asset); corevideo::graphics::GraphicsTemplateRegistry registry;
  const auto baselineBytes=registry.poll()->encodedAssetBytes;
  const auto install = [&](const std::string& request, const Json& document) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    std::string admission;
    do { admission = registry.submit("package-qa",request,document.stringify(),package.root.string());
      if (admission != "template-import-busy") break;
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    } while (std::chrono::steady_clock::now() < deadline);
    EXPECT_EQ("template-install-queued",admission);
    while (std::chrono::steady_clock::now() < deadline) {
      auto publication = registry.poll();
      for (const auto& row : publication->installs.asArray())
        if (row.getString("requestId") == request && (row.getString("state") == "installed" || row.getString("state") == "failed")) return row.getString("state");
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return std::string("timed-out");
  };
  auto document = graphicsTest::imageDocument(asset).asObject();
  EXPECT_EQ("installed",install("first",document));
  const auto first = registry.poll()->definitions.at({"packaged-logo",1})->assets().at("logo")->encoded;
  EXPECT_EQ(baselineBytes+asset->encoded->size(),registry.poll()->encodedAssetBytes);
  document["templateVersion"] = 2; EXPECT_EQ("installed",install("second",document));
  EXPECT_EQ(first,registry.poll()->definitions.at({"packaged-logo",2})->assets().at("logo")->encoded);
  EXPECT_EQ(baselineBytes+asset->encoded->size(),registry.poll()->encodedAssetBytes);
  { std::ofstream changed(package.file,std::ios::binary|std::ios::trunc); changed << "invalid replacement"; }
  document["templateVersion"] = 3; EXPECT_EQ("failed",install("corrupt",document));
  EXPECT_EQ(0u,registry.poll()->definitions.count({"packaged-logo",3}));
  EXPECT_EQ(first,registry.poll()->definitions.at({"packaged-logo",1})->assets().at("logo")->encoded);
  EXPECT_EQ(baselineBytes+asset->encoded->size(),registry.poll()->encodedAssetBytes);
}

TEST(GraphicsTemplate, TypedNumericFieldsDriveActualLayoutAndTypographyWithoutMixedUnits) {
  auto object = manifest(); auto fields = object["fields"].asArray();
  fields.emplace_back(Json::Object{{"id","size"},{"type","number"},{"default",42},{"minimum",16},{"maximum",120}}); object["fields"] = fields;
  auto layers = object["layers"].asArray(); auto group = layers[1].asObject(); auto children = group["children"].asArray();
  auto name = children[0].asObject(); name["fontSize"] = Json::Object{{"field","size"}}; children[0] = name;
  group["children"] = children; layers[1] = group; object["layers"] = layers;
  std::string error; auto definition = load(object,error); ASSERT_TRUE(definition);
  auto values = definition->defaultFields().asObject(); values["name"] = "Guest"; values["size"] = 63.123456789;
  GraphicsDrawing drawing; ASSERT_TRUE(definition->resolve(values,brand(),drawing,error));
  EXPECT_EQ(63.123456789,drawing.primitives[1].fontSize);
  values["size"] = 121; EXPECT_FALSE(definition->resolve(values,brand(),drawing,error));
}

TEST(GraphicsTemplate, BooleanVisibilityHasAConsumerAndUnusedRequiredFieldsAreRefused) {
  auto object = manifest(); auto fields = object["fields"].asArray();
  fields.emplace_back(Json::Object{{"id","show-title"},{"type","boolean"},{"default",true}}); object["fields"] = fields;
  auto layers = object["layers"].asArray(); auto group = layers[1].asObject(); auto children = group["children"].asArray();
  auto title = children[1].asObject(); title["visible"] = Json::Object{{"field","show-title"}}; children[1] = title;
  group["children"] = children; layers[1] = group; object["layers"] = layers;
  std::string error; auto definition = load(object,error); ASSERT_TRUE(definition);
  auto values = definition->defaultFields().asObject(); values["name"] = "Guest"; values["title"] = "Engineer";
  GraphicsDrawing drawing; ASSERT_TRUE(definition->resolve(values,brand(),drawing,error)); EXPECT_EQ(3u,drawing.primitives.size());
  values["show-title"] = false; ASSERT_TRUE(definition->resolve(values,brand(),drawing,error)); EXPECT_EQ(2u,drawing.primitives.size());
  fields.emplace_back(Json::Object{{"id","unreachable"},{"type","string"},{"default",""},{"required",true}}); object["fields"] = fields;
  EXPECT_FALSE(load(object,error)); EXPECT_EQ("required-template-field-unused",error);
}

TEST(GraphicsTemplate, EveryCuratedLookResolvesToNativePrimitivesWithVerifiedLogoAndPlacement) {
  const auto& looks=GraphicsTemplate::curated(); ASSERT_EQ(6u,looks.size());
  std::string error;
  for (const auto& definition : looks) {
    auto fields=definition->defaultFields().asObject(); if (fields.count("name")) fields["name"]="Ada";
    GraphicsDrawing drawing; ASSERT_TRUE(definition->resolve(fields,brand(),drawing,error)); EXPECT_FALSE(drawing.primitives.empty());
    if (definition->id()!="corporate-lower-third") EXPECT_TRUE(definition->defaultPlacement().has_value());
    if (definition->id()=="corner-logo" || definition->id()=="logo-accent") {
      EXPECT_EQ(1u,definition->assets().size());
      EXPECT_TRUE(std::any_of(drawing.primitives.begin(),drawing.primitives.end(),[](const auto& p){return p.image!=nullptr;}));
    }
  }
}

TEST(GraphicsTemplateDraft, PrivateCompilerCoalescesWithoutInstallingAndFencesHashes) {
  corevideo::graphics::GraphicsTemplateRegistry installed; const auto initial=installed.poll();
  auto compiler=std::make_shared<corevideo::graphics::GraphicsTemplateDraft>(); std::string key;
  auto object=manifest(); auto layers=object["layers"].asArray(); auto plate=layers[0].asObject(); plate["fill"]="#FF0000"; layers[0]=plate; object["layers"]=layers;
  const auto source=Json(object).stringify(); const auto bytes=corevideo::modules::hashing::sha256(reinterpret_cast<const uint8_t*>(source.data()),source.size());
  std::string hash;static constexpr char digits[]="0123456789abcdef";for(auto b:bytes) {hash+=digits[b>>4];hash+=digits[b&15];}
  ASSERT_TRUE(compiler->submit(source,hash,GraphicsTemplate::corporateLowerThird(),key));
  const auto until=std::chrono::steady_clock::now()+std::chrono::seconds(5);
  while((!compiler->poll() || compiler->poll()->key!=key) && std::chrono::steady_clock::now()<until) std::this_thread::sleep_for(std::chrono::milliseconds(1));
  auto publication=compiler->poll(); ASSERT_TRUE(publication);ASSERT_TRUE(publication->definition); EXPECT_TRUE(publication->error.empty());
  auto input=std::make_shared<corevideo::graphics::GraphicsTemplateInput>(); input->definition=GraphicsTemplate::corporateLowerThird();input->draft=compiler;input->draftKey=key;
  auto fields=input->definition->defaultFields().asObject();fields["name"]="Guest";input->fields=fields;input->brand=brand();
  std::string reason;auto resolved=corevideo::graphics::resolveGraphicsTemplateInput(input,reason);ASSERT_TRUE(resolved);
  GraphicsDrawing drawing;ASSERT_TRUE(resolved->definition->resolve(resolved->fields,resolved->brand,drawing,reason)); EXPECT_EQ(0xffff0000u,drawing.primitives[0].color);
  EXPECT_EQ(resolved,corevideo::graphics::resolveGraphicsTemplateInput(input,reason));
  ASSERT_TRUE(compiler->submit(source,std::string(64,'0'),GraphicsTemplate::corporateLowerThird(),key));
  while((!compiler->poll() || compiler->poll()->key!=key) && std::chrono::steady_clock::now()<until) std::this_thread::sleep_for(std::chrono::milliseconds(1));
  ASSERT_TRUE(compiler->poll());EXPECT_EQ("private-template-hash-mismatch",compiler->poll()->error);
  EXPECT_EQ(initial,installed.poll());
}

TEST(GraphicsTemplateRegistry, ThumbnailsAreExplicitWorkerProductsWithoutInstallingVersions) {
  corevideo::graphics::GraphicsTemplateRegistry registry;
  const auto before=registry.poll();
  ASSERT_TRUE(registry.requestThumbnails());
  const auto until=std::chrono::steady_clock::now()+std::chrono::seconds(20);
  std::shared_ptr<const corevideo::graphics::GraphicsTemplateRegistry::Publication> result;
  bool complete=false;
  do {
    result=registry.poll();complete=true;
    for(const auto& row:result->catalog.asArray()) if(!row.get("thumbnail") || row.get("thumbnail")->getString("state")=="not-requested" || row.get("thumbnail")->getString("state")=="pending") complete=false;
    if(!complete) std::this_thread::sleep_for(std::chrono::milliseconds(10));
  } while(!complete && std::chrono::steady_clock::now()<until);
  ASSERT_TRUE(complete);EXPECT_EQ(before->definitions,result->definitions);EXPECT_EQ(before->encodedAssetBytes,result->encodedAssetBytes);
  if(const auto* exportPath=std::getenv("COREVIDEO_GRAPHICS_CATALOG_EXPORT");exportPath && *exportPath) {
    std::ofstream artifact(exportPath,std::ios::binary);ASSERT_TRUE(artifact.good());artifact<<result->catalog.stringify();artifact.flush();ASSERT_TRUE(artifact.good());
  }
  for(const auto& row:result->catalog.asArray()) {
    const auto* thumb=row.get("thumbnail");ASSERT_TRUE(thumb);
#if !COREVIDEO_STUB && COREVIDEO_ENABLE_DEV_ADAPTERS && COREVIDEO_WITH_D3D11
    EXPECT_EQ(thumb->getString("state"),"ready") << row.getString("templateId") << ": " << thumb->getString("reason");
    EXPECT_EQ(thumb->getString("backend"),"d3d11-native");EXPECT_TRUE(thumb->getString("pngBase64").rfind("iVBORw0KGgo",0)==0);
    EXPECT_TRUE(thumb->getNumber("width")<=320);EXPECT_TRUE(thumb->getNumber("height")<=180);
#else
    EXPECT_EQ(thumb->getString("state"),"unsupported");
#endif
  }
}
