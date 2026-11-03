#include "graphics/GraphicsTemplate.h"
#include "graphics/GraphicsTemplateRegistry.h"
#include "graphics/GraphicsTemplateDraft.h"
#include "modules/Sha256.h"
#include "gtest/gtest.h"
#include "GraphicsAssetFixture.h"
#include "GraphicsMotionFixture.h"
#include <chrono>
#include <thread>
#include <fstream>
#include <algorithm>
#include <filesystem>
#include <sstream>
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

// Designer layer structure (Slice 2): a hidden group draws none of its children, and the shell-only layer name and
// lock under node metadata survive native load untouched (no native change backs these edits).
TEST(GraphicsTemplate, HiddenGroupDrawsNoChildrenAndKeepsEditorMetadata) {
  std::ifstream in(std::filesystem::path(COREVIDEO_REPO_ROOT) / "native" / "tests" / "fixtures" / "graphics-editor" / "designer-structure.json", std::ios::binary);
  ASSERT_TRUE(in.good());
  std::ostringstream text; text << in.rdbuf();
  std::string error; const auto definition = GraphicsTemplate::load(text.str(), error); ASSERT_TRUE(definition) << error;
  GraphicsDrawing drawing;
  ASSERT_TRUE(definition->resolve(Json::Object{{"name","Ada"},{"title","Host"},{"image",""},{"show-title",true}}, brand(), drawing, error)) << error;
  for (const auto& primitive : drawing.primitives) { EXPECT_NE("name-label", primitive.id); EXPECT_NE("title-label", primitive.id); }
  EXPECT_FALSE(drawing.primitives.empty());
  const auto group = std::find_if(drawing.nodes.begin(), drawing.nodes.end(), [](const auto& node) { return node.id == "group-1"; });
  ASSERT_NE(group, drawing.nodes.end()); EXPECT_FALSE(group->visible);
  const auto& layers = definition->document().get("layers")->asArray();
  const auto stored = std::find_if(layers.begin(), layers.end(), [](const auto& node) { return node.getString("id") == "group-1"; });
  ASSERT_NE(stored, layers.end()); const auto* metadata = stored->get("metadata"); ASSERT_NE(metadata, nullptr);
  EXPECT_EQ("Name block", metadata->getString("label"));
  ASSERT_NE(metadata->get("editorLocked"), nullptr); EXPECT_TRUE(metadata->get("editorLocked")->asBool());
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

namespace {
// Graphics Designer slice 1: unused imported versions free their registry slot.
Json::Object oneLayerDocument(const std::string& id, int version) {
  return Json::Object{{"schemaVersion",1},{"templateId",id},{"templateVersion",version},{"name","Removable"},{"family","bug"},
      {"canvas",Json::Object{{"width",1920},{"height",1080}}},{"bounds",Json::Object{{"width",100},{"height",100}}},
      {"brandTokens",Json::Array{"brand.primary"}},{"fields",Json::Array{}},
      {"layers",Json::Array{Json::Object{{"id","plate"},{"type","rect"},{"x",0},{"y",0},{"width",100},{"height",100},
          {"fill",Json::Object{{"token","brand.primary"}}}}}},
      {"animations",Json::Object{{"in",Json::Object{{"type","none"}}},{"out",Json::Object{{"type","none"}}}}}};
}
std::string installAndWait(corevideo::graphics::GraphicsTemplateRegistry& registry, const std::string& request, const Json::Object& document) {
  const auto bytes=Json(document).stringify();
  const auto limit=std::chrono::steady_clock::now()+std::chrono::seconds(5);
  std::string submitted;
  do { submitted=registry.submit("qa",request,bytes); if(submitted!="template-import-busy") break; std::this_thread::sleep_for(std::chrono::milliseconds(1)); }
  while(std::chrono::steady_clock::now()<limit);
  if(submitted!="template-install-queued") return "submit-"+submitted;
  while(std::chrono::steady_clock::now()<limit) {
    const auto publication=registry.poll(); // keep the snapshot alive for the whole loop
    for(const auto& row:publication->installs.asArray()) if(row.getString("requestId")==request) {
      const auto state=row.getString("state");
      if(state=="installed") return "installed";
      if(state=="failed") return "failed-"+row.getString("reason");
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  return "timed-out";
}
std::string removeWhenIdle(corevideo::graphics::GraphicsTemplateRegistry& registry, const std::string& id, int version) {
  const auto limit=std::chrono::steady_clock::now()+std::chrono::seconds(5);
  std::string result;
  do { result=registry.remove(id,version); if(result!="template-library-busy") return result; std::this_thread::sleep_for(std::chrono::milliseconds(1)); }
  while(std::chrono::steady_clock::now()<limit);
  return result;
}
bool catalogLists(const corevideo::graphics::GraphicsTemplateRegistry& registry, const std::string& id, int version) {
  for(const auto& row:registry.poll()->catalog.asArray()) if(row.getString("templateId")==id && row.getNumber("templateVersion")==version) return true;
  return false;
}
}  // namespace

TEST(GraphicsTemplateRegistry, RemoveFreesTheSlotAndBytes) {
  corevideo::graphics::GraphicsTemplateRegistry registry;
  const auto baseline=registry.poll();
  const auto builtIns=baseline->definitions.size();
  ASSERT_EQ("installed",installAndWait(registry,"remove-me",oneLayerDocument("remove-me",1)));
  ASSERT_TRUE(catalogLists(registry,"remove-me",1));
  EXPECT_TRUE(registry.poll()->documentBytes>baseline->documentBytes);
  EXPECT_EQ("template-removed",removeWhenIdle(registry,"remove-me",1));
  EXPECT_FALSE(catalogLists(registry,"remove-me",1));
  EXPECT_FALSE(registry.poll()->definitions.contains({"remove-me",1}));
  EXPECT_EQ(builtIns,registry.poll()->definitions.size());
  EXPECT_EQ(baseline->documentBytes,registry.poll()->documentBytes);
  // The freed slot is real: the library fills to its 16-definition cap again.
  for(size_t i=0;builtIns+i<16;++i) {
    const auto id="refill-"+std::to_string(i);
    EXPECT_EQ("installed",installAndWait(registry,id,oneLayerDocument(id,1))) << id;
  }
  EXPECT_EQ(16u,registry.poll()->definitions.size());
  EXPECT_EQ("failed-template-library-capacity",installAndWait(registry,"overflow",oneLayerDocument("overflow",1)));
  EXPECT_EQ(10u,16-builtIns);
}

TEST(GraphicsTemplateRegistry, RemoveRefusesBuiltIns) {
  corevideo::graphics::GraphicsTemplateRegistry registry;
  const auto before=registry.poll();
  EXPECT_EQ("template-built-in",removeWhenIdle(registry,"corner-logo",1));
  EXPECT_EQ("template-built-in",removeWhenIdle(registry,"broadcast",1));
  for(const auto& builtin:GraphicsTemplate::curated()) EXPECT_EQ("template-built-in",removeWhenIdle(registry,builtin->id(),builtin->version())) << builtin->id();
  EXPECT_TRUE(catalogLists(registry,"corner-logo",1));
  EXPECT_EQ(before->definitions,registry.poll()->definitions);
}

TEST(GraphicsTemplateRegistry, RemoveUnknownIsNotFound) {
  corevideo::graphics::GraphicsTemplateRegistry registry;
  EXPECT_EQ("template-not-found",removeWhenIdle(registry,"never-installed",1));
  ASSERT_EQ("installed",installAndWait(registry,"only-two",oneLayerDocument("only-two",2)));
  EXPECT_EQ("template-not-found",removeWhenIdle(registry,"only-two",1));
  EXPECT_EQ("template-not-found",removeWhenIdle(registry,"only-two",0));
  EXPECT_TRUE(catalogLists(registry,"only-two",2));
  EXPECT_EQ("template-removed",removeWhenIdle(registry,"only-two",2));
  EXPECT_EQ("template-not-found",removeWhenIdle(registry,"only-two",2));
}

TEST(GraphicsTemplate, TemplatesWithoutLayerMotionHaveNoUnits) {
  for (const auto& definition : GraphicsTemplate::curated()) {
    EXPECT_FALSE(definition->motionSummary().layered) << definition->id();
  }
  const auto definition = GraphicsTemplate::corporateLowerThird();
  EXPECT_FALSE(definition->motionSummary().layered);
  GraphicsDrawing drawing;
  std::string error;
  ASSERT_TRUE(definition->resolve(Json::Object{{"name", "Ada Lovelace"}, {"title", "Engineer"}}, brand(), drawing, error)) << error;
  EXPECT_TRUE(drawing.units.empty());
  for (const auto& primitive : drawing.primitives) EXPECT_EQ(-1, primitive.unit);
}

namespace {
using corevideo::graphics::test::motionFixture;

TEST(GraphicsTemplate, LayerMotionLoadsIntoUnitsAndSummarisesTotals) {
  std::string error;
  const auto bar = GraphicsTemplate::load(motionFixture("bar-first.json"), error);
  ASSERT_TRUE(bar) << error;
  const auto& summary = bar->motionSummary();
  EXPECT_TRUE(summary.layered);
  EXPECT_TRUE(summary.anyFollows);
  EXPECT_EQ(2u, summary.units);
  EXPECT_EQ(500., summary.inMs);
  EXPECT_EQ(500., summary.outMs);
  GraphicsDrawing drawing;
  ASSERT_TRUE(bar->resolve(bar->defaultFields(), brand(), drawing, error)) << error;
  ASSERT_EQ(2u, drawing.units.size());
  EXPECT_EQ("plate", drawing.units[0].nodeId);
  EXPECT_TRUE(drawing.units[0].root);
  EXPECT_EQ(0., drawing.units[0].box.x);
  EXPECT_EQ(1060., drawing.units[0].box.width);
  EXPECT_EQ(168., drawing.units[0].mask.height);
  for (const auto& primitive : drawing.primitives) {
    EXPECT_EQ(primitive.id == "plate" ? 0 : primitive.id == "name-label" ? 1 : -1, primitive.unit);
  }
  const auto nested = GraphicsTemplate::load(motionFixture("nested-motion.json"), error);
  ASSERT_TRUE(nested) << error;
  ASSERT_TRUE(nested->resolve(nested->defaultFields(), brand(), drawing, error)) << error;
  ASSERT_EQ(3u, drawing.units.size());
  EXPECT_EQ(0, drawing.units[1].parent);
  EXPECT_FALSE(drawing.units[1].root);
  EXPECT_EQ(100., drawing.units[1].mask.x);
  EXPECT_EQ(120., drawing.units[1].mask.height);
  EXPECT_TRUE(nested->motionSummary().anyFollows);
}

TEST(GraphicsTemplate, LayerMotionValidationRefusesBadShapesWithExactReasons) {
  using M = Json::Object;
  const auto parsed = Json::parse(motionFixture("bar-first.json"));
  ASSERT_TRUE(parsed);
  auto document = parsed->asObject();
  auto layers = document.at("layers").asArray();
  const auto plate = std::find_if(layers.begin(), layers.end(),
      [](const auto& node) { return node.getString("id") == "plate"; });
  ASSERT_TRUE(plate != layers.end());
  auto node = plate->asObject();
  const auto withPlateAnimations = [&](const Json& animations) {
    node["animations"] = animations;
    *plate = node;
    document["layers"] = layers;
    return Json(document).stringify();
  };
  const std::vector<std::pair<Json, std::string>> cases = {
    {M{{"type","spin"},{"durationMs",100}}, "template-layer-motion"},
    {M{{"type","fade"},{"startMs",-1},{"durationMs",100}}, "template-layer-motion"},
    {M{{"type","fade"},{"durationMs",3001}}, "template-layer-motion"},
    {M{{"type","fade"},{"startMs",2800},{"durationMs",300}}, "template-motion-too-long"},
    {M{{"type","cut"},{"durationMs",100}}, "template-layer-motion"},
    {M{{"type","fade"},{"durationMs",0}}, "template-layer-motion"},
    {M{{"type","fade"},{"durationMs",100},{"direction","left"}}, "template-layer-motion"},
    {M{{"type","rise"},{"durationMs",100},{"distance",0}}, "template-layer-motion"},
    {M{{"type","slide"},{"durationMs",100},{"direction","diagonal"}}, "template-layer-motion"},
    {M{{"type","fade"},{"durationMs",100},{"loop",true}}, "template-layer-motion"}
  };
  std::string error;
  for (const auto& [motion, reason] : cases) {
    EXPECT_FALSE(GraphicsTemplate::load(withPlateAnimations(M{{"in", motion}}), error));
    EXPECT_EQ(reason, error) << motion.stringify();
  }
  EXPECT_FALSE(GraphicsTemplate::load(withPlateAnimations(M{}), error));
  EXPECT_EQ("template-layer-motion", error);
  EXPECT_TRUE(GraphicsTemplate::load(withPlateAnimations(M{{"in", M{{"type","rise"},
      {"durationMs",100},{"distance","edge"}}}}), error)) << error;
  for (int i = 1; i <= 15; ++i) {
    node["id"] = "rect-" + std::to_string(i);
    layers.push_back(node);
  }
  document["layers"] = layers;
  EXPECT_FALSE(GraphicsTemplate::load(Json(document).stringify(), error));
  EXPECT_EQ("template-motion-layer-limit", error);
}
} // namespace

TEST(GraphicsTemplate, EllipseAndLineResolveIntoMotionAwarePrimitives) {
  using M = Json::Object;
  auto document = manifest();
  auto layers = document.at("layers").asArray();
  layers.emplace_back(M{{"id","badge"},{"type","ellipse"},{"x",12},{"y",12},{"width",30},{"height",20},
      {"fill","#FFFFFF"},{"stroke","#0080FF"},{"strokeWidth",2},
      {"animations",M{{"in",M{{"type","fade"},{"durationMs",150}}}}}});
  layers.emplace_back(M{{"id","rule"},{"type","line"},{"x",48},{"y",12},{"width",30},{"height",8},
      {"diagonal","up"},{"stroke","#FFFFFF"},{"strokeWidth",2}});
  document["layers"] = std::move(layers);
  std::string error;
  const auto definition = load(document, error);
  ASSERT_TRUE(definition) << error;
  GraphicsDrawing drawing;
  ASSERT_TRUE(definition->resolve(M{{"name","Ada"},{"title","Engineer"}}, brand(), drawing, error)) << error;
  ASSERT_GE(drawing.primitives.size(), 2u);
  const auto& ellipse = drawing.primitives[drawing.primitives.size()-2];
  const auto& line = drawing.primitives.back();
  EXPECT_EQ("ellipse", ellipse.type);
  EXPECT_EQ(0, ellipse.unit);
  EXPECT_EQ("line", line.type);
  EXPECT_EQ(8., line.box.height);
}

TEST(GraphicsTemplate, TextOutlineAndShadowAcceptTheirDocumentedLimits) {
  using M = Json::Object;
  const auto edited = [](double outlineWidth, double blur) {
    auto document = manifest();
    auto layers = document.at("layers").asArray();
    auto group = layers.back().asObject();
    auto children = group.at("children").asArray();
    auto text = children.front().asObject();
    text["outline"] = M{{"color","#123456"},{"width",outlineWidth}};
    text["shadow"] = M{{"color","#00000080"},{"x",-2},{"y",4},{"blur",blur}};
    children.front() = std::move(text);
    group["children"] = std::move(children);
    layers.back() = std::move(group);
    document["layers"] = std::move(layers);
    return document;
  };
  std::string error;
  EXPECT_TRUE(load(edited(8, 16), error)) << error;
  EXPECT_FALSE(load(edited(8.1, 16), error));
  EXPECT_FALSE(load(edited(8, 16.1), error));
}

TEST(GraphicsTemplate, AutoWidthFitsVisibleContentAndClampsBeforeMotionLayout) {
  using M = Json::Object;
  auto document = manifest();
  auto layers = document.at("layers").asArray();
  auto plate = layers.front().asObject();
  plate["width"] = "auto";
  plate["minWidth"] = 140;
  plate["maxWidth"] = 240;
  layers.front() = std::move(plate);
  auto group = layers.back().asObject();
  group["width"] = "auto";
  group["minWidth"] = 80;
  group["maxWidth"] = 180;
  layers.back() = std::move(group);
  document["layers"] = std::move(layers);
  std::string error;
  const auto definition = load(document, error);
  ASSERT_TRUE(definition) << error;
  GraphicsDrawing shortName, longName;
  ASSERT_TRUE(definition->resolve(M{{"name","I"},{"title",""}}, brand(), shortName, error)) << error;
  ASSERT_TRUE(definition->resolve(M{{"name","A long presenter name that needs more room"},{"title",""}}, brand(), longName, error)) << error;
  EXPECT_EQ(140., shortName.primitives.front().box.width);
  EXPECT_EQ(204., longName.primitives.front().box.width);
  EXPECT_EQ(180., longName.nodes[1].box.width);
}

TEST(GraphicsTemplate, TextShrinksToItsMinimumBeforeEllipsis) {
  auto document = manifest();
  auto layers = document.at("layers").asArray();
  auto group = layers.back().asObject();
  auto children = group.at("children").asArray();
  auto text = children.front().asObject();
  text["width"] = 60;
  text["minFontSize"] = 20;
  children.front() = std::move(text);
  group["children"] = std::move(children);
  layers.back() = std::move(group);
  document["layers"] = std::move(layers);
  std::string error;
  const auto definition = load(document, error);
  ASSERT_TRUE(definition) << error;
  GraphicsDrawing drawing;
  ASSERT_TRUE(definition->resolve(Json::Object{{"name","WWWWWWWWWWWWWW"},{"title",""}}, brand(), drawing, error)) << error;
  ASSERT_EQ(2u, drawing.primitives.size());
  EXPECT_EQ(20., drawing.primitives.back().fontSize);
  EXPECT_EQ("ellipsis", drawing.primitives.back().overflow);
}

TEST(GraphicsTemplate, PrivateDraftLoadsNewVerifiedPackageAssets) {
  using M = Json::Object;
  std::string error;
  const auto asset = corevideo::graphics::graphicsAssetFromBytes("draft-logo", graphicsTest::png(), error);
  ASSERT_TRUE(asset) << error;
  graphicsTest::Package package(asset);
  const auto parent = GraphicsTemplate::corporateLowerThird();
  auto document = parent->document().asObject();
  auto layers = document.at("layers").asArray();
  layers.emplace_back(M{{"id","imported-logo"},{"type","image"},{"asset",asset->id},
      {"x",0},{"y",0},{"width",80},{"height",40},{"fit","contain"}});
  document["layers"] = std::move(layers);
  document["assets"] = Json::Array{M{{"id",asset->id},{"file","assets/" + asset->sha256 + ".png"},{"sha256",asset->sha256}}};
  const auto bytes = Json(document).stringify();
  const auto hash = corevideo::modules::hashing::sha256(reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size());
  std::string hex;
  for (const auto byte : hash) { hex += "0123456789abcdef"[byte >> 4]; hex += "0123456789abcdef"[byte & 15]; }
  corevideo::graphics::GraphicsTemplateDraft draft;
  std::string key;
  ASSERT_TRUE(draft.submit(bytes, hex, parent, key, M{{"name","Ada"},{"title","Engineer"}}, brand(), package.root.string()));
  const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(3);
  auto published = draft.poll();
  while ((!published || published->key != key) && std::chrono::steady_clock::now() < until) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
    published = draft.poll();
  }
  ASSERT_TRUE(published);
  ASSERT_TRUE(published->definition) << published->error;
  EXPECT_EQ(asset->sha256, published->definition->assets().at(asset->id)->sha256);
  EXPECT_FALSE(parent->assets().contains(asset->id));
}
