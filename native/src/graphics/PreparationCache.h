#pragma once

#include <atomic>
#include <algorithm>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <thread>

namespace corevideo::graphics {

// One persistent preparation owner. Render callers only read immutable results
// or attempt bounded admission; they never execute or wait for the preparer.
// A failed preparation is retained until the key (including device epoch)
// changes, avoiding a per-frame retry loop for missing fonts/assets/devices.
template <typename Key, typename Request, typename Asset>
class PreparationCache {
 public:
  struct Result {
    std::shared_ptr<const Asset> asset;
    uint64_t bytes = 0;
    std::string warning;
  };
  struct Counters {
    uint64_t admitted = 0, completed = 0, refused = 0, failed = 0;
    uint64_t residentBytes = 0, pendingBytes = 0, highWaterBytes = 0;
  };
  using Prepare = std::function<Result(const Request&)>;
  explicit PreparationCache(Prepare prepare, uint64_t byteBudget = 256ull * 1024 * 1024,
                            size_t capacity = 32, std::function<void()> cleanup = {}, size_t residentCapacity = 0)
      : prepare_(std::move(prepare)), cleanup_(std::move(cleanup)), budget_(byteBudget), capacity_(capacity),
        residentCapacity_(residentCapacity ? residentCapacity : capacity),
        published_(std::make_shared<const Results>()), worker_([this] { run(); }) {}
  ~PreparationCache() {
    { std::lock_guard lock(mutex_); stopping_ = true; pending_.clear(); }
    wake_.notify_one();
    if (worker_.joinable()) worker_.join();
  }
  PreparationCache(const PreparationCache&) = delete;
  PreparationCache& operator=(const PreparationCache&) = delete;

  std::shared_ptr<const Result> find(const Key& key) const {
    const auto snapshot = std::atomic_load(&published_);
    const auto found = snapshot->find(key);
    if (found == snapshot->end()) return {};
    found->second->lastUse.store(++useClock_, std::memory_order_relaxed);
    return found->second->result;
  }

  std::shared_ptr<const Result> findOrRequest(const Key& key, Request request, uint64_t maximumBytes) {
    if (auto result = find(key)) return result;
    std::unique_lock lock(mutex_, std::try_to_lock);
    if (!lock || stopping_) { refused_.fetch_add(1); return {}; }
    // Completion can publish between the first lookup and admission. A second
    // check prevents duplicate allocation and resident-byte double accounting.
    if (auto result = find(key)) return result;
    if (queued_.count(key)) return {};
    if (maximumBytes == 0 || maximumBytes > budget_ || pending_.size() >= capacity_ ||
        pendingBytes_ > budget_ - maximumBytes) { refused_.fetch_add(1); return {}; }
    queued_.insert(key);
    pending_.push_back({key, std::move(request), maximumBytes});
    pendingBytes_ += maximumBytes;
    admitted_.fetch_add(1);
    lock.unlock(); wake_.notify_one();
    return {};
  }

  Counters counters() const {
    std::lock_guard lock(mutex_);
    return {admitted_.load(), completed_.load(), refused_.load(), failed_.load(),
            residentBytes_, pendingBytes_, highWaterBytes_};
  }

 private:
  struct Entry {
    std::shared_ptr<const Result> result;
    mutable std::atomic<uint64_t> lastUse{0};
    Entry(std::shared_ptr<const Result> value, uint64_t use)
        : result(std::move(value)), lastUse(use) {}
  };
  using Results = std::map<Key, std::shared_ptr<Entry>>;
  struct Job { Key key; Request request; uint64_t maximumBytes; };
  void run() {
    struct Cleanup {
      std::function<void()>& action;
      ~Cleanup() { if (action) { try { action(); } catch (...) {} } }
    } cleanup{cleanup_};
    Results results;
    std::deque<std::pair<std::weak_ptr<const Result>, uint64_t>> retired;
    const auto sweepRetired = [&] {
      for (auto it = retired.begin(); it != retired.end();) {
        if (it->first.expired()) { residentBytes_ -= it->second; it = retired.erase(it); }
        else ++it;
      }
    };
    const auto retire = [&](const Key& key) {
      if (auto found = results.find(key); found != results.end()) {
        retired.emplace_back(found->second->result, found->second->result->bytes);
        results.erase(found);
      }
    };
    const auto retireLeastUsed = [&] {
      auto oldest = std::min_element(results.begin(), results.end(), [](const auto& a, const auto& b) {
        return a.second->lastUse.load(std::memory_order_relaxed) <
               b.second->lastUse.load(std::memory_order_relaxed);
      });
      if (oldest != results.end()) retire(oldest->first);
    };
    for (;;) {
      std::optional<Job> job;
      {
        std::unique_lock lock(mutex_);
        wake_.wait(lock, [this] { return stopping_ || !pending_.empty(); });
        if (stopping_) return;
        job.emplace(std::move(pending_.front())); pending_.pop_front();
        sweepRetired();
        // Only the owner evicts; leases held by render callers survive safely.
        // Reserve scratch + result space before running the producer.
        while (!results.empty() && (residentBytes_ + pendingBytes_ > budget_ ||
                                   results.size() + retired.size() >= residentCapacity_)) {
          retireLeastUsed();
          std::atomic_store(&published_, std::make_shared<const Results>(results));
          sweepRetired();
        }
        if (residentBytes_ + pendingBytes_ > budget_ || results.size() + retired.size() >= residentCapacity_) {
          pendingBytes_ -= job->maximumBytes; queued_.erase(job->key);
          refused_.fetch_add(1); job.reset();
        } else highWaterBytes_ = std::max(highWaterBytes_, residentBytes_ + pendingBytes_);
        std::atomic_store(&published_, std::make_shared<const Results>(results));
      }
      if (!job) continue;
      Result result;
      try { result = prepare_(job->request); }
      catch (...) { result.warning = "Graphics preparation failed."; }
      if (result.bytes > job->maximumBytes) {
        result.asset.reset(); result.bytes = 0; result.warning = "Graphics preparation exceeded its admitted budget.";
      }
      if (!result.asset) { result.bytes = 0; failed_.fetch_add(1); }
      {
        std::lock_guard lock(mutex_);
        pendingBytes_ -= job->maximumBytes; queued_.erase(job->key);
        if (stopping_) return;
        residentBytes_ += result.bytes;
        results[job->key] = std::make_shared<Entry>(
            std::make_shared<const Result>(std::move(result)), ++useClock_);
        // Failed entries are bounded too, even though their GPU footprint is zero.
        while (results.size() > residentCapacity_) {
          retireLeastUsed();
        }
        std::atomic_store(&published_, std::make_shared<const Results>(results));
        sweepRetired();
        completed_.fetch_add(1);
      }
    }
  }
  Prepare prepare_;
  std::function<void()> cleanup_;
  const uint64_t budget_;
  const size_t capacity_;
  const size_t residentCapacity_;
  mutable std::mutex mutex_;
  std::condition_variable wake_;
  std::deque<Job> pending_;
  std::set<Key> queued_;
  bool stopping_ = false;
  uint64_t residentBytes_ = 0, pendingBytes_ = 0, highWaterBytes_ = 0;
  std::atomic<uint64_t> admitted_{0}, completed_{0}, refused_{0}, failed_{0};
  mutable std::atomic<uint64_t> useClock_{0};
  std::shared_ptr<const Results> published_;
  std::thread worker_;
};
}  // namespace corevideo::graphics
