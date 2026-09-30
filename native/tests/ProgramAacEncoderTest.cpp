#include "modules/ProgramAacEncoder.h"

#include <gtest/gtest.h>

#include <cmath>
#include <vector>

#if defined(_WIN32)
TEST(ProgramAacEncoder, EmitsSampleCountedAdtsAtFortyEightKhzStereo) {
  corevideo::modules::ProgramAacEncoder encoder;
  ASSERT_TRUE(encoder.start());
  std::vector<corevideo::modules::ProgramAacPacket> all;
  for (int block = 0; block < 50; ++block) {
    std::vector<float> pcm(960 * 2);
    for (int frame = 0; frame < 960; ++frame) {
      const float value = (block == 10 && frame == 0) ? 0.95f : 0.0f;
      pcm[frame * 2] = value;
      pcm[frame * 2 + 1] = value;
    }
    std::vector<corevideo::modules::ProgramAacPacket> packets;
    ASSERT_TRUE(encoder.encode(pcm, 2, 48000, packets));
    all.insert(all.end(), std::make_move_iterator(packets.begin()), std::make_move_iterator(packets.end()));
  }
  ASSERT_GE(all.size(), 40u);
  for (size_t i = 0; i < all.size(); ++i) {
    ASSERT_GE(all[i].adts.size(), 7u);
    EXPECT_EQ(all[i].adts[0], 0xff);
    EXPECT_EQ(all[i].adts[1] & 0xf0, 0xf0);
    if (i) EXPECT_EQ(all[i].sampleIndex - all[i - 1].sampleIndex, 1024);
  }
}
#endif
