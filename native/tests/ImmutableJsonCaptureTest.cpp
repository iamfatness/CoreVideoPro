#include "rpc/Json.h"
#include <gtest/gtest.h>

using corevideo::rpc::Json;
TEST(ImmutableJsonCapture, CopiesLeaseTheSameTreeAndEditingACopyCannotChangeCapturedValues) {
  Json current = Json::Object{{"scene", "old"}, {"rows", Json::Array{Json::Object{{"name","Guest"}}}}};
  const auto capture = current;
  EXPECT_EQ(&current.asObject(), &capture.asObject());
  auto edited = current.asObject(); edited["scene"] = "new";
  current = std::move(edited);
  EXPECT_EQ(capture.getString("scene"), "old"); EXPECT_EQ(current.getString("scene"), "new");
  EXPECT_EQ(&current.get("rows")->asArray(), &capture.get("rows")->asArray());
}
TEST(ImmutableJsonCapture, MoveLeavesAValidNullInsteadOfADanglingContainer) {
  Json first = Json::Object{{"value", "payload"}};
  Json second(std::move(first)); EXPECT_TRUE(first.isNull());
  EXPECT_EQ(second.getString("value"), "payload");
  first = std::move(second); EXPECT_TRUE(second.isNull());
  EXPECT_EQ(first.getString("value"), "payload");
}
