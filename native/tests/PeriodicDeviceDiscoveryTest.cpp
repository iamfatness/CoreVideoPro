#include "modules/PeriodicDeviceDiscovery.h"
#include <gtest/gtest.h>
#include <future>
#include <vector>

using corevideo::modules::PeriodicDeviceDiscovery;
using namespace std::chrono_literals;

TEST(PeriodicDeviceDiscovery, BlockedDriverDoesNotBlockSnapshotReader) {
  std::promise<void> entered, release;
  auto gate = release.get_future().share();
  PeriodicDeviceDiscovery<std::vector<int>> discovery([&]() -> std::optional<std::vector<int>> {
    entered.set_value();
    gate.wait();
    return std::vector<int>{7};
  }, 1h);
  entered.get_future().wait();
  auto reader = std::async(std::launch::async, [&] { return discovery.takeLatest(); });
  const bool ready = reader.wait_for(2s) == std::future_status::ready;
  // Always release the fake driver, including on failure, so test cleanup cannot hang.
  release.set_value();
  EXPECT_TRUE(ready);
  if (ready) EXPECT_FALSE(static_cast<bool>(reader.get()));
}

TEST(PeriodicDeviceDiscovery, FailedRefreshDoesNotEraseUnconsumedResult) {
  std::promise<void> failed;
  int calls = 0;
  PeriodicDeviceDiscovery<std::vector<int>> discovery([&]() -> std::optional<std::vector<int>> {
    if (++calls == 1) return std::vector<int>{3, 8};
    if (calls == 2) failed.set_value();
    return std::nullopt;
  }, 1ms);
  failed.get_future().wait();
  auto result = discovery.takeLatest();
  ASSERT_TRUE(static_cast<bool>(result));
  EXPECT_EQ(result->size(), static_cast<size_t>(2));
  EXPECT_EQ(result->at(0), 3);
  EXPECT_FALSE(static_cast<bool>(discovery.takeLatest()));
}

TEST(PeriodicDeviceDiscovery, SuccessfulEmptyDiscoveryPublishesRemoval) {
  std::promise<void> entered;
  PeriodicDeviceDiscovery<std::vector<int>> discovery([&]() -> std::optional<std::vector<int>> {
    entered.set_value();
    return std::vector<int>{};
  }, 1h);
  entered.get_future().wait();
  std::unique_ptr<std::vector<int>> result;
  const auto deadline = std::chrono::steady_clock::now() + 2s;
  while (!(result = discovery.takeLatest()) && std::chrono::steady_clock::now() < deadline)
    std::this_thread::yield();
  ASSERT_TRUE(static_cast<bool>(result));
  EXPECT_TRUE(result->empty());
}
