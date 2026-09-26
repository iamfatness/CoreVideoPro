#include "core/AvSyncClapProbe.h"

#include <gtest/gtest.h>

namespace {

corevideo::modules::VideoFrame whiteFrame() {
  corevideo::modules::VideoFrame frame;
  frame.participantId = "101";
  frame.i420Width = 4;
  frame.i420Height = 4;
  frame.i420 = std::make_shared<const std::vector<uint8_t>>(24, 235);
  return frame;
}

TEST(AvSyncClapProbe, WhiteClapRequiresTheRoutedSourceAndFullLuma) {
  auto frame = whiteFrame();
  EXPECT_TRUE(corevideo::core::hasWhiteClapSource({frame}, "101"));
  EXPECT_FALSE(corevideo::core::hasWhiteClapSource({frame}, "102"));

  auto dark = std::vector<uint8_t>(24, 235);
  dark[2 * 4 + 2] = 80;
  frame.i420 = std::make_shared<const std::vector<uint8_t>>(std::move(dark));
  EXPECT_FALSE(corevideo::core::hasWhiteClapSource({frame}, "101"));
}

}  // namespace
