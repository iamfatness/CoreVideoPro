#include "graphics/GraphicsTemplate.h"
#include "graphics/GraphicsTemplateRegistry.h"
#include "gtest/gtest.h"
#include <chrono>
#include <thread>

using corevideo::graphics::GraphicsTemplate;
using corevideo::graphics::GraphicsDrawing;
using corevideo::rpc::Json;
namespace {
Json::Object manifest() { return GraphicsTemplate::corporateLowerThird()->document().asObject(); }
Json brand() { return Json::Object{{"brand.primary", "#10203080"}, {"brand.onPrimary", "#FFFFFF"},
    {"brand.onPrimaryMuted", "#CCDDFF"}, {"brand.fontBody", "Segoe UI"}}; }
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
