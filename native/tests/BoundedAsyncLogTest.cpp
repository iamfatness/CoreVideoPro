#include "core/BoundedAsyncLog.h"
#include <gtest/gtest.h>
#include <chrono>
#include <condition_variable>
#include <future>
#include <mutex>
#include <string>
#include <thread>
#if !defined(_WIN32)
#include <unistd.h>
#include <fcntl.h>
#endif

namespace {
// These tests have one producer. The logger intentionally uses try_lock, so
// startup contention may drop a write before the sink ever sees it. Retry ONLY
// that confirmed drop: retrying an accepted write would invoke one-shot promises
// (or close a descriptor) twice and invalidate the assertions below.
bool writeOnceAccepted(corevideo::core::BoundedAsyncLog& logger, std::string_view message) {
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  do {
    const auto before = logger.stats().dropped;
    logger.write(message);
    if (logger.stats().dropped == before) return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  } while (std::chrono::steady_clock::now() < deadline);
  return false;
}
}

TEST(BoundedAsyncLog, BlockedSinkCannotBlockProducerOrShutdownAndQueueStaysBounded) {
  using corevideo::core::BoundedAsyncLog;
  struct SinkState {
    std::mutex mutex;
    std::condition_variable changed;
    bool entered = false, released = false, exited = false;
  };
  auto state = std::make_shared<SinkState>();
  auto logger = std::make_unique<BoundedAsyncLog>([state](std::string_view) {
    std::unique_lock<std::mutex> lock(state->mutex);
    state->entered = true;
    state->changed.notify_all();
    state->changed.wait(lock, [&] { return state->released; });
    state->exited = true;
    state->changed.notify_all();
    return true;
  });
  const bool accepted = writeOnceAccepted(*logger, "first\n");
  const auto startupDrops = logger->stats().dropped;
  bool entered;
  {
    std::unique_lock<std::mutex> lock(state->mutex);
    entered = state->changed.wait_for(lock, std::chrono::seconds(2), [&] { return state->entered; });
  }
  auto producer = std::async(std::launch::async, [&] {
    for (int i = 0; i < 10000; ++i) logger->write("diagnostic\n");
    const auto stats = logger->stats();
    logger.reset(); // Must finish while the sink remains blocked.
    return stats;
  });
  const bool completedWhileBlocked = producer.wait_for(std::chrono::seconds(2)) == std::future_status::ready;
  // Always release the test sink before assertions (MiniGTest throws).
  {
    std::lock_guard<std::mutex> lock(state->mutex);
    state->released = true;
  }
  state->changed.notify_all();
  const auto stats = producer.get();
  EXPECT_TRUE(accepted);
  EXPECT_TRUE(entered);
  EXPECT_TRUE(completedWhileBlocked);
  EXPECT_EQ(stats.queued, BoundedAsyncLog::kCapacity);
  EXPECT_EQ(stats.dropped - startupDrops, 10000u - BoundedAsyncLog::kCapacity);
}

TEST(BoundedAsyncLog, OversizedMessagesAreTruncatedAndSinkFailuresAreCounted) {
  using corevideo::core::BoundedAsyncLog;
  auto result = std::make_shared<std::promise<std::string>>();
  auto future = result->get_future();
  BoundedAsyncLog logger([result](std::string_view message) {
    result->set_value(std::string(message));
    return false;
  });
  ASSERT_TRUE(writeOnceAccepted(logger, std::string(10000, 'x')));
  ASSERT_TRUE(future.wait_for(std::chrono::seconds(2)) == std::future_status::ready);
  const auto message = future.get();
  EXPECT_EQ(message.size(), BoundedAsyncLog::kMessageBytes - 1);
  EXPECT_EQ(message.back(), '\n');
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (logger.stats().sinkFailures == 0 && std::chrono::steady_clock::now() < deadline)
    std::this_thread::yield();
  EXPECT_EQ(logger.stats().truncated, 1u);
  EXPECT_EQ(logger.stats().sinkFailures, 1u);
}

TEST(BoundedAsyncLog, VerboseLoggingIsOffUntilExplicitlyEnabled) {
  corevideo::core::setNativeVerboseLoggingEnabled(false);
  EXPECT_FALSE(corevideo::core::nativeVerboseLoggingEnabled());

  corevideo::core::setNativeVerboseLoggingEnabled(true);
  EXPECT_TRUE(corevideo::core::nativeVerboseLoggingEnabled());

  // Do not leak process-global diagnostic state into later tests.
  corevideo::core::setNativeVerboseLoggingEnabled(false);
}

#if !defined(_WIN32)
TEST(BoundedAsyncLog, ClosedPipeIsCountedWithoutTerminatingProcess) {
  int pipeEnds[2];
  ASSERT_EQ(::pipe(pipeEnds), 0);
  ::close(pipeEnds[0]);
  auto completed = std::make_shared<std::promise<void>>();
  auto future = completed->get_future();
  corevideo::core::BoundedAsyncLog logger([writeFd = pipeEnds[1], completed](std::string_view message) {
    const bool written = corevideo::core::writeDiagnosticDescriptor(writeFd, message);
    ::close(writeFd);
    completed->set_value();
    return written;
  });
  const bool accepted = writeOnceAccepted(logger, "closed pipe\n");
  if (!accepted) ::close(pipeEnds[1]); // No queued callback owns it on this path.
  ASSERT_TRUE(accepted);
  ASSERT_TRUE(future.wait_for(std::chrono::seconds(2)) == std::future_status::ready);
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (logger.stats().sinkFailures == 0 && std::chrono::steady_clock::now() < deadline)
    std::this_thread::yield();
  EXPECT_EQ(logger.stats().sinkFailures, 1u);
}
TEST(BoundedAsyncLog, DescriptorSinkWritesBytesAndRejectsInvalidDescriptor) {
  int pipeEnds[2];
  ASSERT_EQ(::pipe(pipeEnds), 0);
  const bool written = corevideo::core::writeDiagnosticDescriptor(pipeEnds[1], "message");
#if defined(__APPLE__)
  const int noSigpipe = ::fcntl(pipeEnds[1], F_GETNOSIGPIPE);
#endif
  char bytes[7]{};
  const auto received = written ? ::read(pipeEnds[0], bytes, sizeof(bytes)) : -1;
  ::close(pipeEnds[0]);
  ::close(pipeEnds[1]);
  EXPECT_TRUE(written);
  EXPECT_EQ(received, 7);
  EXPECT_EQ(std::string(bytes, sizeof(bytes)), std::string("message"));
#if defined(__APPLE__)
  EXPECT_EQ(noSigpipe, 1);
#endif
  EXPECT_FALSE(corevideo::core::writeDiagnosticDescriptor(-1, "invalid"));
}
#endif
