#include "core/FrameAllocation.h"
#include "modules/NdiReceiveFramePolicy.h"

#include <gtest/gtest.h>

#include <limits>

namespace {
using namespace corevideo::core;

// A size no vector can hold, so the request fails inside std::vector (length_error)
// and never reaches operator new. A merely huge size would: sanitizer builds abort on
// that instead of throwing.
constexpr std::size_t kImpossible = (std::numeric_limits<std::size_t>::max)();

TEST(FrameAllocation, ABufferIsMadeLikeMakeShared) {
  const auto sized = tryMakeFrameBuffer(std::size_t{64});
  ASSERT_TRUE(sized);
  EXPECT_EQ(sized->size(), 64u);

  const std::uint8_t bytes[] = {1, 2, 3, 4};
  const auto ranged = tryMakeFrameBuffer(bytes, bytes + 4);
  ASSERT_TRUE(ranged);
  EXPECT_EQ(*ranged, (FrameBytes{1, 2, 3, 4}));

  const auto copied = tryMakeFrameBuffer(*ranged);
  ASSERT_TRUE(copied);
  EXPECT_EQ(*copied, *ranged);
}

// #728: the failure the core died of, three times, on 2026-10-01.
TEST(FrameAllocation, AnAllocationThatCannotBeMadeReturnsNullAndDoesNotThrow) {
  EXPECT_FALSE(tryMakeFrameBuffer(kImpossible));
}

TEST(FrameAllocation, AFailedResizeLeavesTheBufferAsItWas) {
  FrameBytes buffer{9, 9, 9};
  EXPECT_FALSE(tryResizeFrameBuffer(buffer, kImpossible));
  EXPECT_EQ(buffer, (FrameBytes{9, 9, 9}));
  EXPECT_TRUE(tryResizeFrameBuffer(buffer, 5));
  EXPECT_EQ(buffer.size(), 5u);
}

TEST(FrameAllocation, FailuresAreAllCountedButLoggedAtMostEveryFiveSeconds) {
  FrameAllocationFailures failures("test-site");
  EXPECT_TRUE(failures.noteAt(100, 1'000'000));   // the first one is always said
  EXPECT_FALSE(failures.noteAt(100, 1'000'016));  // a starved machine fails every frame
  EXPECT_FALSE(failures.noteAt(100, 1'004'999));
  EXPECT_TRUE(failures.noteAt(100, 1'005'000));
  EXPECT_EQ(failures.count(), 4u);
}

// One real consumer end to end: an NDI frame with valid geometry still copies.
TEST(FrameAllocation, TheNdiCopyStillProducesAFrame) {
  const std::uint8_t bgra[2 * 2 * 4] = {1, 2, 3, 0, 4, 5, 6, 0, 7, 8, 9, 0, 10, 11, 12, 0};
  const auto copy = corevideo::modules::copyNdiBgra(bgra, 2, 2, 8, /*opaque=*/true);
  ASSERT_TRUE(copy);
  ASSERT_EQ(copy->size(), 16u);
  EXPECT_EQ((*copy)[0], 1);
  EXPECT_EQ((*copy)[3], 255);
}

}  // namespace
