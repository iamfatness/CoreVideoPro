// ZComms' assertion macros (tests/audio/test_util.h @ 168a572) re-expressed on
// Pro's gtest shim, so the ported ZComms tests compile unchanged. A failed
// ZC_CHECK is a Pro test failure with ZComms' own case name attached.
// (Edge's copy maps the same macros onto its TestHarness; the test bodies are
// identical in both repositories.)
#pragma once

#include <cmath>
#include <cstdio>
#include <string>

#include <gtest/gtest.h>

namespace zctest {
inline const char*& CurrentTest() {
  static const char* t = "";
  return t;
}
}  // namespace zctest

#define ZC_CHECK(cond) EXPECT_TRUE(cond) << ::zctest::CurrentTest() << ": " #cond

#define ZC_CHECK_NEAR(actual, expected, tol)                                                       \
  do {                                                                                             \
    const double a_ = (actual), e_ = (expected), t_ = (tol);                                       \
    EXPECT_TRUE(std::fabs(a_ - e_) <= t_) << ::zctest::CurrentTest() << ": " #actual " = " << a_  \
                                          << ", expected " << e_ << " +/- " << t_;                 \
  } while (0)

#define ZC_TEST(name) ::zctest::CurrentTest() = name;

void TestEnvelope();
void TestLimiter();
void TestFrameAccumulator();
void TestSampleRing();
void TestFrameRing();
void TestRoomPlan();
void TestReach();
void TestTalkbackEligibility();
void TestDuckPlan();
void TestTalkbackChannels();
void TestSignalGate();
