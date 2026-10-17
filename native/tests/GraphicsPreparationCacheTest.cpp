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
void admit(Cache& cache, int key, int value, uint64_t bytes) {
  const auto accepted = cache.counters().admitted + 1;
  // Production admission is intentionally try-lock and may refuse while the
  // worker starts or publishes. A refused attempt is not an admitted job.
  ASSERT_TRUE(eventually([&] {
    if (cache.counters().admitted >= accepted) return true;
    cache.findOrRequest(key, value, bytes);
    return cache.counters().admitted >= accepted;
  }));
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
  ASSERT_TRUE(eventually([&] { cache.findOrRequest(1, 10, 8); return cache.counters().admitted == 1; }));
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
  admit(cache, 1, 10, 60);
  ASSERT_TRUE(eventually([&] { return bool(cache.find(1)); }));
  auto held = cache.find(1);
  admit(cache, 2, 20, 60);
  ASSERT_TRUE(eventually([&] { return cache.counters().pendingBytes == 0; }));
  EXPECT_EQ(1, prepared.load());
  EXPECT_EQ(40u, cache.counters().residentBytes);
  EXPECT_EQ(10, *held->asset);
  held.reset();
  admit(cache, 2, 20, 60);
  ASSERT_TRUE(eventually([&] { return bool(cache.find(2)); }));
  EXPECT_EQ(2, prepared.load());
  EXPECT_EQ(40u, cache.counters().residentBytes);
  EXPECT_LE(cache.counters().highWaterBytes, 80u);
}

TEST(GraphicsPreparationCache, ExceptionsAreStickyAndDoNotRetryEveryFrame) {
  std::atomic<int> prepared{0};
  Cache cache([&](int) -> Cache::Result { prepared++; throw std::runtime_error("not-for-logs"); });
  admit(cache, 1, 10, 8);
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
  admit(cache, 1, 1, 8);
  const bool running = eventually([&] { return started.load(); });
  EXPECT_TRUE(running);
  cache.findOrRequest(2, 2, 8);
  const auto priorRefusals = cache.counters().refused;
  cache.findOrRequest(3, 3, 8);
  cache.findOrRequest(4, 4, 65);
  EXPECT_EQ(2u, cache.counters().admitted);
  EXPECT_EQ(priorRefusals + 2, cache.counters().refused);
  unblock.release();
}

TEST(GraphicsPreparationCache, ReadHitsProtectTheRecentlyUsedEntry) {
  Cache cache([](int value) { return Cache::Result{std::make_shared<int>(value), 4, ""}; }, 64, 2);
  admit(cache, 1, 1, 8);
  ASSERT_TRUE(eventually([&] { return cache.counters().completed == 1; }));
  admit(cache, 2, 2, 8);
  ASSERT_TRUE(eventually([&] { return cache.counters().completed == 2; }));
  ASSERT_TRUE(cache.find(1));
  admit(cache, 3, 3, 8);
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
    admit(cache, 1, 1, 8);
    ASSERT_TRUE(eventually([&] { return cache.find(1) != nullptr; }));
  }
  EXPECT_TRUE(cleaned);
  EXPECT_TRUE(producer == cleanup);
  EXPECT_TRUE(producer != std::this_thread::get_id());
}

TEST(GraphicsPreparationCache, RetiringResourceCountIsBoundedEvenForTinyAssets) {
  Cache cache([](int value) { return Cache::Result{std::make_shared<int>(value), 1, ""}; }, 1024, 4, {}, 2);
  admit(cache, 1, 1, 2);
  ASSERT_TRUE(eventually([&] { return cache.find(1) != nullptr; }));
  auto first = cache.find(1);
  admit(cache, 2, 2, 2);
  ASSERT_TRUE(eventually([&] { return cache.find(2) != nullptr; }));
  auto second = cache.find(2);
  admit(cache, 3, 3, 2);
  ASSERT_TRUE(eventually([&] { return cache.counters().pendingBytes == 0; }));
  EXPECT_FALSE(cache.find(3));
  EXPECT_EQ(2u, cache.counters().completed);
  first.reset(); second.reset();
  admit(cache, 3, 3, 2);
  ASSERT_TRUE(eventually([&] { return cache.find(3) != nullptr; }));
}

TEST(GraphicsPreparationCache, APrivateCueSlotOnlyPreparesItsLatestQueuedEdit) {
  std::promise<void> release;
  const auto gate = release.get_future().share();
  std::atomic<bool> started{false};
  std::atomic<int> prepared{0};
  Cache cache([&](int value) {
    if (value == 1) { started = true; gate.wait(); }
    prepared++;
    return Cache::Result{std::make_shared<int>(value), 4, ""};
  }, 128, 1);
  ReleaseGate unblock{release};
  ASSERT_TRUE(eventually([&] { cache.findOrRequest(1, 1, 8, "cue:featured"); return started.load(); }));
  ASSERT_TRUE(eventually([&] { cache.findOrRequest(2, 2, 8, "cue:featured"); return cache.counters().admitted == 2; }));
  // A full queue can replace its own pending edit without increasing bytes.
  ASSERT_TRUE(eventually([&] { cache.findOrRequest(3, 3, 8, "cue:featured"); return cache.counters().admitted == 3; }));
  EXPECT_EQ(16u, cache.counters().pendingBytes);
  EXPECT_EQ(1u, cache.counters().superseded);
  unblock.release();
  ASSERT_TRUE(eventually([&] { return bool(cache.find(3)); }));
  EXPECT_FALSE(cache.find(1)); EXPECT_FALSE(cache.find(2));
  EXPECT_EQ(2, prepared.load()); // one in-flight producer, one final edit
  EXPECT_EQ(2u, cache.counters().superseded);
  EXPECT_EQ(1u, cache.counters().completed);
  EXPECT_EQ(0u, cache.counters().pendingBytes);
}

TEST(GraphicsPreparationCache, SupersedingOneCueDoesNotCancelAnotherOrTheVisibleProgramResource) {
  std::promise<void> release;
  const auto gate = release.get_future().share();
  std::atomic<bool> started{false};
  Cache cache([&](int value) {
    if (value == 2) { started = true; gate.wait(); }
    return Cache::Result{std::make_shared<int>(value), 4, ""};
  }, 128, 4);
  ReleaseGate unblock{release};
  ASSERT_TRUE(eventually([&] { cache.findOrRequest(1, 1, 8); return bool(cache.find(1)); }));
  const auto visible = cache.find(1);
  ASSERT_TRUE(eventually([&] { cache.findOrRequest(2, 2, 8, "cue:first"); return started.load(); }));
  ASSERT_TRUE(eventually([&] { cache.findOrRequest(3, 3, 8, "cue:second"); return cache.counters().admitted == 3; }));
  ASSERT_TRUE(eventually([&] { cache.findOrRequest(4, 4, 8, "cue:first"); return cache.counters().admitted == 4; }));
  unblock.release();
  ASSERT_TRUE(eventually([&] { return bool(cache.find(3)) && bool(cache.find(4)); }));
  EXPECT_EQ(1, *visible->asset);
  EXPECT_TRUE(cache.find(1)); EXPECT_FALSE(cache.find(2));
  EXPECT_EQ(1u, cache.counters().superseded);
}

TEST(GraphicsPreparationCache, RefusedOversizedEditCannotCancelAdmittedCue) {
  std::promise<void> release;
  const auto gate = release.get_future().share();
  std::atomic<bool> started{false};
  Cache cache([&](int value) {
    started = true; gate.wait();
    return Cache::Result{std::make_shared<int>(value), 4, ""};
  }, 64, 2);
  ReleaseGate unblock{release};
  ASSERT_TRUE(eventually([&] { cache.findOrRequest(1, 1, 8, "cue:first"); return started.load(); }));
  EXPECT_FALSE(cache.findOrRequest(2, 2, 65, "cue:first"));
  unblock.release();
  ASSERT_TRUE(eventually([&] { return bool(cache.find(1)); }));
  EXPECT_EQ(0u, cache.counters().superseded);
  EXPECT_FALSE(cache.find(2));
}

TEST(GraphicsPreparationCache, SupersededRequestReleasesItsDeviceLikeLeaseOnTheWorker) {
  using LeaseCache = corevideo::graphics::PreparationCache<int, std::shared_ptr<int>, int>;
  std::promise<void> release;
  const auto gate = release.get_future().share();
  std::atomic<bool> started{false}, disposed{false};
  std::thread::id producer, disposer;
  LeaseCache cache([&](const std::shared_ptr<int>& value) {
    if (*value == 1) { producer = std::this_thread::get_id(); started = true; gate.wait(); }
    return LeaseCache::Result{std::make_shared<int>(*value), 4, ""};
  }, 128, 4);
  ReleaseGate unblock{release};
  ASSERT_TRUE(eventually([&] { cache.findOrRequest(1, std::make_shared<int>(1), 8, "cue:first"); return started.load(); }));
  auto lease = std::shared_ptr<int>(new int(2), [&](int* value) {
    delete value; disposer = std::this_thread::get_id(); disposed.store(true, std::memory_order_release);
  });
  ASSERT_TRUE(eventually([&] { cache.findOrRequest(2, lease, 8, "cue:first"); return cache.counters().admitted == 2; }));
  lease.reset();
  ASSERT_TRUE(eventually([&] { cache.findOrRequest(3, std::make_shared<int>(3), 8, "cue:first"); return cache.counters().admitted == 3; }));
  EXPECT_FALSE(disposed.load());
  unblock.release();
  ASSERT_TRUE(eventually([&] { return disposed.load(std::memory_order_acquire); }));
  EXPECT_TRUE(disposer == producer);
  EXPECT_TRUE(disposer != std::this_thread::get_id());
}

TEST(GraphicsPreparationCache, AnotherCueProtectsSharedPreparationFromCancellation) {
  for (bool alreadyRunning : {true, false}) {
    std::promise<void> release;
    const auto gate = release.get_future().share();
    std::atomic<bool> started{false};
    const int blocked = alreadyRunning ? 1 : 0;
    Cache cache([&](int value) {
      if (value == blocked) { started = true; gate.wait(); }
      return Cache::Result{std::make_shared<int>(value), 4, ""};
    }, 128, 4);
    ReleaseGate unblock{release};
    ASSERT_TRUE(eventually([&] { cache.findOrRequest(blocked, blocked, 8, alreadyRunning ? "cue:first" : "other"); return started.load(); }));
    if (!alreadyRunning) cache.findOrRequest(1, 1, 8, "cue:first");
    cache.findOrRequest(1, 1, 8, "cue:second"); // same content, independent consumer
    cache.findOrRequest(2, 2, 8, "cue:first");
    unblock.release();
    ASSERT_TRUE(eventually([&] { return bool(cache.find(1)) && bool(cache.find(2)); }));
    EXPECT_EQ(0u, cache.counters().superseded);
    EXPECT_EQ(1, *cache.find(1)->asset);
    EXPECT_EQ(2, *cache.find(2)->asset);
  }
}
