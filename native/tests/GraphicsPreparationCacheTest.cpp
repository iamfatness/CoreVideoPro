#include "graphics/PreparationCache.h"
#include "gtest/gtest.h"
#include <chrono>
#include <future>
#include <stdexcept>

using Cache = corevideo::graphics::PreparationCache<int, int, int>;
namespace {
// mini-gtest throws on failed expectations. Release a blocked producer before
// cache destruction even when an expectation fails, so a failing test terminates.
struct ReleaseGate {
  std::promise<void>& promise;
  bool released = false;
  void release() { if (!released) { released = true; promise.set_value(); } }
  ~ReleaseGate() { release(); }
};
template<class Predicate> bool eventually(Predicate predicate) {
  const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (!predicate() && std::chrono::steady_clock::now() < end) std::this_thread::sleep_for(std::chrono::milliseconds(1));
  return predicate();
}
}

TEST(GraphicsPreparationCache, CallerNeverExecutesOrWaitsForBlockedProducer) {
  std::promise<void> release;
  const auto gate = release.get_future().share();
  std::atomic<int> prepared{0};
  Cache cache([&](int value) { gate.wait(); prepared++; return Cache::Result{std::make_shared<int>(value), 4, ""}; });
  ReleaseGate unblock{release};
  EXPECT_FALSE(cache.findOrRequest(1, 10, 8));
  EXPECT_FALSE(cache.findOrRequest(1, 10, 8));
  EXPECT_EQ(0, prepared.load());
  EXPECT_EQ(1u, cache.counters().admitted);
  unblock.release();
  ASSERT_TRUE(eventually([&] { return bool(cache.find(1)); }));
  auto result = cache.findOrRequest(1, 999, 8);
  ASSERT_TRUE(result && result->asset);
  EXPECT_EQ(10, *result->asset);
  EXPECT_EQ(1, prepared.load());
}

TEST(GraphicsPreparationCache, RetiringLeasesStayChargedUntilTheirLastConsumerReleases) {
  std::atomic<int> prepared{0};
  Cache cache([&](int value) { prepared++; return Cache::Result{std::make_shared<int>(value), 40, ""}; }, 80, 4);
  cache.findOrRequest(1, 10, 60);
  ASSERT_TRUE(eventually([&] { return bool(cache.find(1)); }));
  auto held = cache.find(1);
  cache.findOrRequest(2, 20, 60);
  ASSERT_TRUE(eventually([&] { return cache.counters().refused > 0; }));
  EXPECT_EQ(1, prepared.load());
  EXPECT_EQ(40u, cache.counters().residentBytes);
  EXPECT_EQ(10, *held->asset);
  held.reset();
  cache.findOrRequest(2, 20, 60);
  ASSERT_TRUE(eventually([&] { return bool(cache.find(2)); }));
  EXPECT_EQ(2, prepared.load());
  EXPECT_EQ(40u, cache.counters().residentBytes);
  EXPECT_LE(cache.counters().highWaterBytes, 80u);
}

TEST(GraphicsPreparationCache, ExceptionsAreStickyAndDoNotRetryEveryFrame) {
  std::atomic<int> prepared{0};
  Cache cache([&](int) -> Cache::Result { prepared++; throw std::runtime_error("not-for-logs"); });
  cache.findOrRequest(1, 10, 8);
  ASSERT_TRUE(eventually([&] { return bool(cache.find(1)); }));
  for (int i = 0; i < 100; ++i) EXPECT_TRUE(cache.findOrRequest(1, 10, 8));
  EXPECT_EQ(1, prepared.load());
  EXPECT_FALSE(cache.find(1)->asset);
  EXPECT_EQ("Graphics preparation failed.", cache.find(1)->warning);
  EXPECT_EQ(1u, cache.counters().failed);
}

TEST(GraphicsPreparationCache, PendingAndOversizedRequestsAreBounded) {
  std::promise<void> release;
  const auto gate = release.get_future().share();
  std::atomic<bool> started{false};
  Cache cache([&](int value) { started = true; gate.wait(); return Cache::Result{std::make_shared<int>(value), 4, ""}; }, 64, 1);
  ReleaseGate unblock{release};
  cache.findOrRequest(1, 1, 8);
  const bool running = eventually([&] { return started.load(); });
  EXPECT_TRUE(running);
  cache.findOrRequest(2, 2, 8);
  cache.findOrRequest(3, 3, 8);
  cache.findOrRequest(4, 4, 65);
  EXPECT_EQ(2u, cache.counters().admitted);
  EXPECT_EQ(2u, cache.counters().refused);
  unblock.release();
}

TEST(GraphicsPreparationCache, ReadHitsProtectTheRecentlyUsedEntry) {
  Cache cache([](int value) { return Cache::Result{std::make_shared<int>(value), 4, ""}; }, 64, 2);
  cache.findOrRequest(1, 1, 8);
  ASSERT_TRUE(eventually([&] { return cache.counters().completed == 1; }));
  cache.findOrRequest(2, 2, 8);
  ASSERT_TRUE(eventually([&] { return cache.counters().completed == 2; }));
  ASSERT_TRUE(cache.find(1));
  cache.findOrRequest(3, 3, 8);
  ASSERT_TRUE(eventually([&] { return cache.counters().completed == 3; }));
  EXPECT_TRUE(cache.find(1));
  EXPECT_FALSE(cache.find(2));
  EXPECT_TRUE(cache.find(3));
}

TEST(GraphicsPreparationCache, CleanupRunsOnThePreparationOwnerAfterJobs) {
  std::thread::id producer, cleanup;
  bool cleaned = false;
  {
    Cache cache([&](int value) {
      producer = std::this_thread::get_id();
      return Cache::Result{std::make_shared<int>(value), 4, ""};
    }, 64, 2, [&] { cleanup = std::this_thread::get_id(); cleaned = true; });
    cache.findOrRequest(1, 1, 8);
    ASSERT_TRUE(eventually([&] { return cache.find(1) != nullptr; }));
  }
  EXPECT_TRUE(cleaned);
  EXPECT_TRUE(producer == cleanup);
  EXPECT_TRUE(producer != std::this_thread::get_id());
}

TEST(GraphicsPreparationCache, RetiringResourceCountIsBoundedEvenForTinyAssets) {
  Cache cache([](int value) { return Cache::Result{std::make_shared<int>(value), 1, ""}; }, 1024, 4, {}, 2);
  cache.findOrRequest(1, 1, 2);
  ASSERT_TRUE(eventually([&] { return cache.find(1) != nullptr; }));
  auto first = cache.find(1);
  cache.findOrRequest(2, 2, 2);
  ASSERT_TRUE(eventually([&] { return cache.find(2) != nullptr; }));
  auto second = cache.find(2);
  cache.findOrRequest(3, 3, 2);
  ASSERT_TRUE(eventually([&] { return cache.counters().refused > 0; }));
  EXPECT_FALSE(cache.find(3));
  EXPECT_EQ(2u, cache.counters().completed);
  first.reset(); second.reset();
  cache.findOrRequest(3, 3, 2);
  ASSERT_TRUE(eventually([&] { return cache.find(3) != nullptr; }));
}
