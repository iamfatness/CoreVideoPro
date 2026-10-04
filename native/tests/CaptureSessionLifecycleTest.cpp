#include "modules/CaptureSessionLifecycle.h"
#include <gtest/gtest.h>
#include <atomic>
#include <chrono>
#include <future>

namespace {
struct Session { int epoch = 0; };
using Lifecycle = corevideo::modules::CaptureSessionLifecycle<Session>;
template<class Predicate> bool await(Predicate predicate) {
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
  while (!predicate() && std::chrono::steady_clock::now() < deadline)
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  return predicate();
}
}

TEST(CaptureSessionLifecycle, BlockedCreationCoalescesRequestsAndNeverPublishesStaleEpoch) {
  std::promise<void> creating, release; auto started = creating.get_future();
  const auto gate = release.get_future().share();
  std::atomic<int> created{0}, retired{0};
  const auto caller = std::this_thread::get_id(); std::atomic<bool> ownerThread{true};
  Lifecycle lifecycle([&](const std::string&) {
    ownerThread.store(ownerThread.load() && std::this_thread::get_id() != caller);
    const int epoch = ++created;
    if (epoch == 1) { creating.set_value(); gate.wait(); }
    return std::make_shared<Session>(Session{epoch});
  }, [&](Session&) { ++retired; });
  EXPECT_TRUE(lifecycle.connect("a"));
  const bool began = started.wait_for(std::chrono::seconds(3)) == std::future_status::ready;
  const bool disconnected = lifecycle.disconnect("a");
  const bool reconnect = lifecycle.connect("a");
  const bool empty = lifecycle.snapshot().empty();
  release.set_value(); // no assertion may leave a blocked owner behind
  const bool connected = await([&] { return lifecycle.status("a") == "connected"; });
  EXPECT_TRUE(began); EXPECT_TRUE(disconnected); EXPECT_TRUE(reconnect); EXPECT_TRUE(empty);
  EXPECT_TRUE(connected); EXPECT_TRUE(ownerThread.load());
  const auto sessions = lifecycle.snapshot();
  ASSERT_EQ(sessions.size(), 1u);
  EXPECT_EQ(sessions.at("a")->epoch, 2);
  EXPECT_EQ(created.load(), 2); EXPECT_EQ(retired.load(), 1);
}

TEST(CaptureSessionLifecycle, BlockedRetirementDoesNotBlockPollOrCommandsAndBoundsAdmission) {
  std::promise<void> retiring, release; auto started = retiring.get_future();
  const auto gate = release.get_future().share(); std::atomic<bool> first{true};
  const auto caller = std::this_thread::get_id(); std::atomic<bool> ownerThread{true};
  Lifecycle lifecycle([](const std::string&) { return std::make_shared<Session>(); },
      [&](Session&) {
        ownerThread.store(ownerThread.load() && std::this_thread::get_id() != caller);
        if (first.exchange(false)) { retiring.set_value(); gate.wait(); }
      }, 1);
  EXPECT_TRUE(lifecycle.connect("a"));
  const bool connected = await([&] { return lifecycle.status("a") == "connected"; });
  const bool disconnected = lifecycle.disconnect("a");
  const bool began = started.wait_for(std::chrono::seconds(3)) == std::future_status::ready;
  const bool hidden = lifecycle.snapshot().empty();
  const bool refused = !lifecycle.connect("b");
  release.set_value();
  const bool stopped = await([&] { return lifecycle.status("a") == "detected"; });
  EXPECT_TRUE(connected); EXPECT_TRUE(disconnected); EXPECT_TRUE(began);
  EXPECT_TRUE(hidden); EXPECT_TRUE(refused); EXPECT_TRUE(stopped); EXPECT_TRUE(ownerThread.load());
  EXPECT_TRUE(lifecycle.connect("b"));
  EXPECT_TRUE(await([&] { return lifecycle.status("b") == "connected"; }));
}

TEST(CaptureSessionLifecycle, FailedStartIsVisibleAndExplicitRetryCanRecover) {
  std::atomic<bool> fail{true};
  Lifecycle lifecycle([&](const std::string&) {
    if (fail.load()) return std::shared_ptr<Session>{};
    return std::make_shared<Session>();
  }, [](Session&) {});
  EXPECT_TRUE(lifecycle.connect("a"));
  EXPECT_TRUE(await([&] { return lifecycle.status("a") == "failed"; }));
  EXPECT_TRUE(lifecycle.snapshot().empty());
  fail.store(false); EXPECT_TRUE(lifecycle.connect("a"));
  EXPECT_TRUE(await([&] { return lifecycle.status("a") == "connected"; }));
}

TEST(CaptureSessionLifecycle, RepeatedConnectPreservesPendingAndRunningSession) {
  std::promise<void> creating, release;
  auto started = creating.get_future();
  const auto gate = release.get_future().share();
  std::atomic<int> created{0}, retired{0};
  Lifecycle lifecycle([&](const std::string&) {
    const int epoch = ++created;
    if (epoch == 1) { creating.set_value(); gate.wait(); }
    return std::make_shared<Session>(Session{epoch});
  }, [&](Session&) { ++retired; });
  EXPECT_TRUE(lifecycle.connect("screen:0"));
  const bool began = started.wait_for(std::chrono::seconds(3)) == std::future_status::ready;
  bool accepted = true;
  for (int i = 0; i < 100; ++i) accepted &= lifecycle.connect("screen:0");
  const bool pending = lifecycle.status("screen:0") == "connecting";
  release.set_value(); // unblock before any fatal assertion
  EXPECT_TRUE(began); EXPECT_TRUE(accepted); EXPECT_TRUE(pending);
  ASSERT_TRUE(await([&] { return lifecycle.status("screen:0") == "connected"; }));
  const auto original = lifecycle.snapshot().at("screen:0");
  for (int i = 0; i < 100; ++i) {
    EXPECT_TRUE(lifecycle.connect("screen:0"));
    EXPECT_EQ(lifecycle.status("screen:0"), "connected");
    const auto sessions = lifecycle.snapshot();
    ASSERT_EQ(sessions.size(), 1u);
    EXPECT_EQ(sessions.at("screen:0"), original);
  }
  EXPECT_EQ(created.load(), 1); EXPECT_EQ(retired.load(), 0);
  EXPECT_TRUE(lifecycle.disconnect("screen:0"));
  ASSERT_TRUE(await([&] { return lifecycle.status("screen:0") == "detected"; }));
  EXPECT_TRUE(lifecycle.connect("screen:0"));
  ASSERT_TRUE(await([&] { return lifecycle.status("screen:0") == "connected"; }));
  EXPECT_EQ(lifecycle.snapshot().at("screen:0")->epoch, 2);
  EXPECT_EQ(created.load(), 2); EXPECT_EQ(retired.load(), 1);
}
