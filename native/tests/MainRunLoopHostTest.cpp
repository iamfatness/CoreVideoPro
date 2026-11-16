// runWithMainRunLoop: on Apple the body runs on a worker while the calling
// thread services the main run loop (CoreMediaIO applies camera arrivals there;
// see core/MainRunLoopHost.h). Elsewhere it runs inline. gtest calls these from
// the process's main thread, which is the production shape (src/main.cpp).
#include "core/MainRunLoopHost.h"

#include <gtest/gtest.h>

#include <chrono>
#include <future>
#include <stdexcept>
#include <thread>

#if defined(__APPLE__)
#include <dispatch/dispatch.h>
#include <pthread.h>
#endif

using corevideo::core::runWithMainRunLoop;

TEST(MainRunLoopHost, ReturnsTheBodysResult) {
  EXPECT_EQ(runWithMainRunLoop([] { return 7; }), 7);
}

TEST(MainRunLoopHost, RethrowsTheBodysExceptionOnTheCaller) {
  const auto throwing = []() -> int { throw std::runtime_error("boom"); };
  bool caught = false;
  try {
    runWithMainRunLoop(throwing);
  } catch (const std::runtime_error&) {
    caught = true;
  }
  EXPECT_TRUE(caught);
}

#if defined(__APPLE__)
// The defect: a body blocked for the whole session on the main thread (the
// stdin loop) starved the main run loop. Work posted to the main queue while
// the body is still running must now execute, and the body must be off-main.
TEST(MainRunLoopHost, MainQueueWorkRunsWhileTheBodyIsStillBlocked) {
  ASSERT_NE(pthread_main_np(), 0) << "gtest must call this from the main thread";
  const int result = runWithMainRunLoop([] {
    if (pthread_main_np() != 0) return -1;  // the body must not own the main thread
    auto ran = std::make_shared<std::promise<void>>();
    auto done = ran->get_future();
    dispatch_async(dispatch_get_main_queue(), ^{ ran->set_value(); });
    // Still inside the body: this is the stdin loop, blocked.
    return done.wait_for(std::chrono::seconds(5)) == std::future_status::ready ? 1 : 0;
  });
  EXPECT_EQ(result, 1);
}

TEST(MainRunLoopHost, ReturnsPromptlyOnceTheBodyEnds) {
  const auto start = std::chrono::steady_clock::now();
  runWithMainRunLoop([] { return 0; });
  EXPECT_LT(std::chrono::steady_clock::now() - start, std::chrono::seconds(1));
}
#endif

#if defined(__APPLE__)
// The body used to own the main thread's 8 MiB stack; a std::thread worker
// would get 512 KiB (review M1).
TEST(MainRunLoopHost, TheBodyKeepsTheMainThreadsStackSize) {
  const std::size_t stack = static_cast<std::size_t>(
      runWithMainRunLoop([] { return static_cast<int>(pthread_get_stacksize_np(pthread_self()) / 1024); })) * 1024;
  EXPECT_GE(stack, corevideo::core::kMainRunLoopBodyStackBytes);
}

TEST(MainRunLoopHost, AnLvalueBodyIsAccepted) {
  auto body = [] { return 3; };
  EXPECT_EQ(runWithMainRunLoop(body), 3);
}
#endif
