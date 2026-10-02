#include "modules/MediaFrameReadStep.h"

#include <gtest/gtest.h>

#include <cstring>

namespace {
using namespace corevideo::modules;

// A pipe of whole frames: frame N is `frameBytes` copies of the byte N.
struct FakePipe {
  std::size_t frameBytes;
  int frames;
  std::size_t position = 0;

  std::size_t operator()(std::uint8_t* destination, std::size_t wanted) {
    const std::size_t total = frameBytes * static_cast<std::size_t>(frames);
    if (position >= total) return 0;
    // Short reads, like a real pipe: never more than 100 bytes at a time.
    const std::size_t inFrame = frameBytes - (position % frameBytes);
    const std::size_t count = (std::min)({wanted, inFrame, static_cast<std::size_t>(100)});
    std::memset(destination, static_cast<int>(position / frameBytes) + 1, count);
    position += count;
    return count;
  }
};

MediaFrameBuffer allocate(std::size_t bytes) {
  return std::make_shared<std::vector<std::uint8_t>>(bytes);
}

TEST(MediaFrameReadStep, AWholeFrameIsRead) {
  FakePipe pipe{1000, 1};
  MediaFrameBuffer frame;
  EXPECT_EQ(readMediaFrame(1000, pipe, allocate, frame), MediaFrameReadResult::Frame);
  ASSERT_TRUE(frame);
  EXPECT_EQ(frame->size(), 1000u);
  EXPECT_EQ(frame->front(), 1);
  EXPECT_EQ(frame->back(), 1);
}

// #728, live 2026-10-01: Windows was low on virtual memory, one 8 MB frame buffer
// could not be allocated, and the uncaught std::bad_alloc on the decoder thread
// terminated the whole core three times in 2.5 minutes.
TEST(MediaFrameReadStep, AFailedAllocationDropsThatFrameAndTheNextOneIsStillTheRightPicture) {
  FakePipe pipe{1000, 3};
  int calls = 0;
  auto failSecond = [&](std::size_t bytes) -> MediaFrameBuffer {
    return ++calls == 2 ? nullptr : allocate(bytes);
  };

  MediaFrameBuffer frame;
  EXPECT_EQ(readMediaFrame(1000, pipe, failSecond, frame), MediaFrameReadResult::Frame);
  EXPECT_EQ(frame->front(), 1);

  MediaFrameBuffer dropped;
  EXPECT_EQ(readMediaFrame(1000, pipe, failSecond, dropped), MediaFrameReadResult::Dropped);
  EXPECT_FALSE(dropped);

  // The dropped frame's bytes were consumed, so the stream is still frame-aligned:
  // the next read is frame 3 from its first byte to its last.
  EXPECT_EQ(readMediaFrame(1000, pipe, failSecond, frame), MediaFrameReadResult::Frame);
  EXPECT_EQ(frame->front(), 3);
  EXPECT_EQ(frame->back(), 3);
}

TEST(MediaFrameReadStep, APipeThatClosesMidFrameEnds) {
  FakePipe pipe{1000, 1};
  pipe.position = 400;  // only 600 bytes of the frame remain
  MediaFrameBuffer frame;
  EXPECT_EQ(readMediaFrame(1000, pipe, allocate, frame), MediaFrameReadResult::Ended);
  EXPECT_FALSE(frame);
}

TEST(MediaFrameReadStep, APipeThatClosesWhileDroppingEnds) {
  FakePipe pipe{1000, 1};
  pipe.position = 400;
  MediaFrameBuffer frame;
  auto fail = [](std::size_t) -> MediaFrameBuffer { return nullptr; };
  EXPECT_EQ(readMediaFrame(1000, pipe, fail, frame), MediaFrameReadResult::Ended);
}

TEST(MediaFrameReadStep, TheProductionAllocatorReturnsNullInsteadOfThrowing) {
  // A size no vector can hold. The point is that it comes back as a null buffer on
  // the calling thread, not as an exception nothing catches. Deliberately not a
  // merely huge size: that reaches operator new, and a sanitizer build aborts on an
  // oversized request instead of throwing (the native-stub-tsan job did).
  EXPECT_FALSE(tryAllocateMediaFrame(static_cast<std::size_t>(-1)));
  const auto small = tryAllocateMediaFrame(64);
  ASSERT_TRUE(small);
  EXPECT_EQ(small->size(), 64u);
}

// #728: the clip reader allocated 8.3 MB per frame, about 500 MB a second.
TEST(MediaFrameReadStep, ARetiredFrameNobodyElseHoldsIsReused) {
  MediaFrameRecycler recycler;
  auto first = recycler.take(1000);
  ASSERT_TRUE(first);
  const auto* storage = first->data();
  recycler.offer(std::move(first));

  const auto second = recycler.take(1000);
  ASSERT_TRUE(second);
  EXPECT_EQ(second->data(), storage);
  EXPECT_EQ(recycler.reused(), 1u);
}

// The compositor may still be drawing (or holding, for a paused clip) the retired frame.
// Writing the next picture into it would tear what is on air.
TEST(MediaFrameReadStep, ARetiredFrameSomeoneStillHoldsIsNeverReused) {
  MediaFrameRecycler recycler;
  auto first = recycler.take(1000);
  const MediaFrameBuffer heldByTheCompositor = first;
  recycler.offer(std::move(first));

  const auto second = recycler.take(1000);
  ASSERT_TRUE(second);
  EXPECT_NE(second->data(), heldByTheCompositor->data());
  EXPECT_EQ(recycler.reused(), 0u);
}

TEST(MediaFrameReadStep, ARetiredFrameOfAnotherSizeIsNotReused) {
  MediaFrameRecycler recycler;
  recycler.offer(recycler.take(500));
  const auto next = recycler.take(1000);
  ASSERT_TRUE(next);
  EXPECT_EQ(next->size(), 1000u);
  EXPECT_EQ(recycler.reused(), 0u);
}

}  // namespace
