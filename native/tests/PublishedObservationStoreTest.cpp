#include "core/AudioOutputObservation.h"
#include "core/AudioObservationFootprint.h"
#include <gtest/gtest.h>
#include <future>
#include <latch>
#include <thread>
#include <vector>

using namespace corevideo::core;

TEST(PublishedObservationStore, LeasedVersionsStayImmutableAndExhaustionDoesNotGrowPool) {
  PublishedObservationStore<int> store;
  std::vector<PublishedObservationStore<int>::View> leases;
  for (int revision = 1; revision <= 4; ++revision) {
    ASSERT_TRUE(store.publish(revision, [&](auto& value) { value = revision; }));
    leases.push_back(store.acquire());
  }
  bool built = false;
  EXPECT_FALSE(store.publish(5, [&](auto&) { built = true; }));
  EXPECT_FALSE(built);
  EXPECT_EQ(store.acquire().refused, 1u);
  EXPECT_EQ(store.acquire().payload->value, 4);
  for (int index = 0; index < 4; ++index) EXPECT_EQ(leases[index].payload->value, index + 1);
  leases.erase(leases.begin());
  ASSERT_TRUE(store.publish(5, [&](auto& value) { value = 5; }));
  EXPECT_EQ(store.acquire().payload->revision, 5u);
}

TEST(PublishedObservationStore, StalledOwnerConstructionDoesNotBlockReaders) {
  PublishedObservationStore<int> store;
  ASSERT_TRUE(store.publish(1, [](auto& value) { value = 42; }));
  std::latch entered(1), release(1);
  std::thread owner([&] { store.publish(2, [&](auto& value) {
    entered.count_down(); release.wait(); value = 99;
  }); });
  entered.wait();
  auto reader = std::async(std::launch::async, [&] { return store.acquire(); });
  const auto ready = reader.wait_for(std::chrono::seconds(2));
  release.count_down(); owner.join();
  ASSERT_EQ(ready, std::future_status::ready);
  EXPECT_EQ(reader.get().payload->value, 42);
  EXPECT_EQ(store.acquire().payload->value, 99);
}

TEST(PublishedObservationStore, FreshnessUsesOwnerTimeAndRetainedValuesSurviveStaleness) {
  AudioObservationStore store;
  const auto start = AudioObservationStore::Clock::time_point(std::chrono::seconds(1));
  EXPECT_EQ(audioObservationMetadata(store.acquire(), "process-a", start).getString("status"), "unobserved");
  ASSERT_TRUE(store.publish(8, [](auto& value) { value.shortTermLufs = -16.3; }, start));
  const auto view = store.acquire();
  EXPECT_EQ(audioObservationMetadata(view, "process-a", start + std::chrono::milliseconds(250)).getString("status"), "fresh");
  EXPECT_EQ(audioObservationMetadata(view, "process-a", start + std::chrono::milliseconds(251)).getString("status"), "aging");
  EXPECT_EQ(audioObservationMetadata(view, "process-a", start + std::chrono::milliseconds(1001)).getString("status"), "stale");
  EXPECT_EQ(view.payload->value.shortTermLufs, -16.3);
  EXPECT_EQ(view.payload->revision, 8u);
}

TEST(PublishedObservationStore, FailedConstructionRetainsPreviousPublication) {
  PublishedObservationStore<int> store;
  ASSERT_TRUE(store.publish(1, [](auto& value) { value = 42; }));
  bool threw = false;
  try { store.publish(2, [](auto&) { throw std::runtime_error("construction failed"); }); }
  catch (const std::runtime_error&) { threw = true; }
  EXPECT_TRUE(threw);
  EXPECT_EQ(store.acquire().payload->value, 42);
  EXPECT_EQ(store.acquire().refused, 1u);
  EXPECT_TRUE(store.publish(3, [](auto& value) { value = 99; }));
}

TEST(PublishedObservationStore, OversizedDraftIsRefusedAndRetiredWithoutLosingPreviousFacts) {
  AudioObservationStore store;
  ASSERT_TRUE(store.publish(1, [](auto& value) {
    value.encoder.recordingSessionId = "take-1";
    return audioObservationBytes(value);
  }));
  EXPECT_FALSE(store.publish(2, [](auto& value) {
    value.encoder.recordingError.assign(512 * 1024, 'x');
    return audioObservationBytes(value);
  }));
  const auto view = store.acquire();
  EXPECT_EQ(view.refused, 1u);
  EXPECT_EQ(view.payload->revision, 1u);
  EXPECT_EQ(view.payload->value.encoder.recordingSessionId, "take-1");
  EXPECT_LT(view.residentBytes, 4u * 512u * 1024u);
  EXPECT_TRUE(store.publish(3, [](auto& value) { return audioObservationBytes(value); }));
}

TEST(PublishedObservationStore, CopiedAndMovedLeasesPinPayloadUntilFinalReaderAndOutliveStore) {
  using Store = PublishedObservationStore<std::vector<int>, 2>;
  Store::View retained;
  {
    Store store;
    ASSERT_TRUE(store.publish(1, [](auto& value) { value.assign(128, 7); }));
    auto first = store.acquire();
    auto copy = first;
    retained = std::move(copy);
    first.payload.reset();
    ASSERT_TRUE(store.publish(2, [](auto& value) { value.assign(128, 8); }));
    EXPECT_FALSE(store.publish(3, [](auto& value) { value.assign(128, 9); }));
    EXPECT_EQ(retained.payload->value.front(), 7);
  }
  EXPECT_EQ(retained.payload->value.back(), 7);
  EXPECT_EQ(retained.payload->revision, 1u);
}

TEST(PublishedObservationStore, ConcurrentReaderRetirementSynchronizesPayloadReuse) {
  PublishedObservationStore<std::vector<uint64_t>> store;
  ASSERT_TRUE(store.publish(1, [](auto& value) { value.assign(128, 1); }));
  std::atomic<bool> invalid{false};
  std::latch start(5);
  std::vector<std::thread> readers;
  for (int index = 0; index < 4; ++index) readers.emplace_back([&] {
    start.count_down(); start.wait();
    for (int pass = 0; pass < 4000; ++pass) {
      const auto held = store.acquire();
      auto copy = held;
      if (!copy.payload || copy.payload->value.size() != 128) { invalid.store(true); continue; }
      for (const auto value : copy.payload->value)
        if (value != copy.payload->revision) invalid.store(true);
    }
  });
  start.count_down(); start.wait();
  for (uint64_t revision = 2; revision < 8000; ++revision)
    (void)store.publish(revision, [revision](auto& value) { value.assign(128, revision); });
  for (auto& reader : readers) reader.join();
  EXPECT_FALSE(invalid.load());
  EXPECT_TRUE(store.publish(8000, [](auto& value) { value.assign(128, 8000); }));
  EXPECT_EQ(store.acquire().payload->revision, 8000u);
}
