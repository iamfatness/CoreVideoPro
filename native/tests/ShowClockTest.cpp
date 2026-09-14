#include "core/ShowClock.h"
#include <gtest/gtest.h>
#include <limits>
#include <stdexcept>

using corevideo::core::ShowClock;

TEST(ShowClock, ExactSixtyFpsBoundariesAndSelectableProgramDelay) {
  ShowClock clock({"show",1,1'000,60,1,48'000});
  EXPECT_EQ(clock.slotAt(1'000),0);
  EXPECT_EQ(clock.slotAt(16'667'666),0);
  EXPECT_EQ(clock.slotAt(16'667'667),1);
  EXPECT_EQ(clock.slotStartNs(3'600),60'000'001'000LL);
  EXPECT_EQ(clock.deliveryDeadlineNs(0,2),33'334'334);
  EXPECT_EQ(clock.deliveryDeadlineNs(0,3),50'001'000);
  EXPECT_FALSE(clock.deliveryDeadlineNs(0,1).has_value());
  EXPECT_FALSE(clock.deliveryDeadlineNs(-1,2).has_value());
  EXPECT_FALSE(clock.deliveryDeadlineNs(-2,2).has_value());
  EXPECT_FALSE(clock.deliveryDeadlineNs(-3,3).has_value());
}

TEST(ShowClock, FractionalRateNeverAccumulatesPeriodRoundingDrift) {
  ShowClock clock({"ntsc",1,0,60'000,1'001,48'000});
  ASSERT_TRUE(clock.slotStartNs(60'000).has_value());
  EXPECT_EQ(*clock.slotStartNs(60'000),1'001'000'000'000LL);
  EXPECT_EQ(clock.slotAt(*clock.slotStartNs(60'000)),60'000);
  EXPECT_EQ(clock.slotAt(*clock.slotStartNs(60'000)-1),59'999);
}

TEST(ShowClock, AudioRangesShareTheVideoAnchorAndCoverLongRunsExactly) {
  ShowClock clock({"show",1,0,60,1,48'000});
  std::int64_t samples = 0;
  for (int slot=0; slot<3'600; ++slot) {
    const auto range = clock.audioRangeForSlot(slot);
    ASSERT_TRUE(range.has_value());
    EXPECT_EQ(range->firstSample,samples);
    samples += range->sampleCount;
  }
  EXPECT_EQ(samples,2'880'000);
  EXPECT_EQ(clock.audioSampleAt(60'000'000'000LL),2'880'000);
}

TEST(ShowClock, FractionalVideoSlotsPartitionIntegerAudioWithoutGaps) {
  ShowClock clock({"ntsc",4,0,60'000,1'001,48'000});
  std::int64_t next = 0;
  bool saw800=false, saw801=false;
  for (int slot=0; slot<1'000; ++slot) {
    const auto range = clock.audioRangeForSlot(slot);
    ASSERT_TRUE(range.has_value());
    EXPECT_EQ(range->firstSample,next);
    next += range->sampleCount;
    saw800 = saw800 || range->sampleCount == 800;
    saw801 = saw801 || range->sampleCount == 801;
  }
  EXPECT_TRUE(saw800); EXPECT_TRUE(saw801);
}

TEST(ShowClock, PreAnchorAndDiscontinuityAreExplicitGenerations) {
  ShowClock clock({"show",8,100,60,1,48'000});
  EXPECT_EQ(clock.slotAt(99),-1);
  EXPECT_EQ(clock.audioSampleAt(99),-1);
  EXPECT_FALSE(clock.audioRangeForSlot(-1).has_value());
  const auto restarted = clock.restart(1'000);
  ASSERT_TRUE(restarted.has_value());
  EXPECT_EQ(restarted->config().generation,9ULL);
  EXPECT_EQ(restarted->config().anchorNs,1'000);
  EXPECT_EQ(clock.config().generation,8ULL);
}

TEST(ShowClock, InvalidConfigurationAndOverflowFailClosed) {
  const auto invalid = [](ShowClock::Config config) {
    try { ShowClock clock(std::move(config)); (void)clock; }
    catch (const std::invalid_argument&) { return true; }
    return false;
  };
  EXPECT_TRUE(invalid({"",1,0,60,1,48'000}));
  EXPECT_TRUE(invalid({"show",0,0,60,1,48'000}));
  EXPECT_TRUE(invalid({"show",1,0,0,1,48'000}));
  ShowClock clock({"show",9'007'199'254'740'991ULL,0,60,1,48'000});
  EXPECT_FALSE(clock.restart(1).has_value());
  EXPECT_FALSE(clock.slotStartNs((std::numeric_limits<std::int64_t>::max)()).has_value());
}
