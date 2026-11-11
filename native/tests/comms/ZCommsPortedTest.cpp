// Registers the ported ZComms test functions as Edge test cases.
#include "test_util.h"

TEST(ZCommsPorted, TalkbackChannels) { TestTalkbackChannels(); }
TEST(ZCommsPorted, TalkbackEligibility) { TestTalkbackEligibility(); }
TEST(ZCommsPorted, DuckPlan) { TestDuckPlan(); }
TEST(ZCommsPorted, Reach) { TestReach(); }
TEST(ZCommsPorted, RoomPlan) { TestRoomPlan(); }
TEST(ZCommsPorted, Envelope) { TestEnvelope(); }
TEST(ZCommsPorted, Limiter) { TestLimiter(); }
TEST(ZCommsPorted, FrameAccumulator) { TestFrameAccumulator(); }
TEST(ZCommsPorted, SampleRing) { TestSampleRing(); }
TEST(ZCommsPorted, FrameRing) { TestFrameRing(); }
TEST(ZCommsPorted, SignalGate) { TestSignalGate(); }
