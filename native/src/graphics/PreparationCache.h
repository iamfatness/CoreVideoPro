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
    bool retryable = false;
  };
  struct Counters {
    uint64_t admitted = 0, completed = 0, refused = 0, failed = 0;
    uint64_t residentBytes = 0, pendingBytes = 0, highWaterBytes = 0;
    uint64_t superseded = 0;
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

  std::shared_ptr<const Result> findOrRequest(const Key& key, Request request, uint64_t maximumBytes,
                                            const std::string& slot = {}) {
    if (auto result = find(key)) return result;
    std::unique_lock lock(mutex_, std::try_to_lock);
    if (!lock || stopping_) { refused_.fetch_add(1); return {}; }
    // Completion can publish between the first lookup and admission. A second
    // check prevents duplicate allocation and resident-byte double accounting.
    if (auto result = find(key)) return result;
    if (queued_.count(key)) {
      // A content key can serve more than one consumer. Once another slot
      // needs it, one editor cannot cancel that shared preparation.
      if (activeKey_ && *activeKey_ == key && activeSlot_ != slot) {
        activeShared_ = true; activeSuperseded_ = false;
      }
      for (auto& job : pending_) if (job.key == key && job.slot != slot) job.shared = true;
      return {};
    }
    // A slot is a private preparation consumer, never its visible Program
    // revision. Validate before superseding so a refused edit cannot cancel
    // otherwise valid work. Metadata remains bounded by the pending jobs;
    // there is no unbounded latest-key map or retained request history.
    if (maximumBytes == 0 || maximumBytes > budget_ || slot.size() > 128) {
      refused_.fetch_add(1); return {};
    }
    uint64_t replacedBytes = 0;
    size_t replacedCount = 0;
    if (!slot.empty()) for (const auto& job : pending_) if (job.slot == slot && !job.shared) {
      replacedBytes += job.maximumBytes; ++replacedCount;
    }
    if (pending_.size() - replacedCount >= capacity_ ||
        pendingBytes_ - replacedBytes > budget_ - maximumBytes ||
        retiredRequests_.size() + replacedCount > capacity_) {
      refused_.fetch_add(1); return {};
    }
    if (!slot.empty()) {
      for (auto it = pending_.begin(); it != pending_.end();) {
        if (it->slot != slot || it->shared) { ++it; continue; }
        queued_.erase(it->key); pendingBytes_ -= it->maximumBytes;
        // Request payloads can hold an old device lease. Their final release
        // belongs on the preparation owner too, never the render caller.
        retiredRequests_.push_back(std::move(*it));
        it = pending_.erase(it); superseded_.fetch_add(1);
      }
      if (activeSlot_ == slot && !activeShared_) activeSuperseded_ = true;
    }
    queued_.insert(key);
    pending_.push_back({key, std::move(request), maximumBytes, slot});
    pendingBytes_ += maximumBytes;
    admitted_.fetch_add(1);
    lock.unlock(); wake_.notify_one();
    return {};
  }

  Counters counters() const {
    std::lock_guard lock(mutex_);
    return {admitted_.load(), completed_.load(), refused_.load(), failed_.load(),
            residentBytes_, pendingBytes_, highWaterBytes_, superseded_.load()};
  }

 private:
  struct Entry {
    std::shared_ptr<const Result> result;
    mutable std::atomic<uint64_t> lastUse{0};
    Entry(std::shared_ptr<const Result> value, uint64_t use)
        : result(std::move(value)), lastUse(use) {}
  };
  using Results = std::map<Key, std::shared_ptr<Entry>>;
  struct Job { Key key; Request request; uint64_t maximumBytes; std::string slot; bool shared = false; };
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
      auto oldest = results.end();
      for (auto it = results.begin(); it != results.end(); ++it) {
        if (it->second->result.use_count() != 1) continue;
        if (oldest == results.end() || it->second->lastUse.load(std::memory_order_relaxed) <
            oldest->second->lastUse.load(std::memory_order_relaxed)) oldest = it;
      }
      if (oldest == results.end()) return false;
      retire(oldest->first); return true;
    };
    for (;;) {
      std::optional<Job> job;
      std::deque<Job> retiredRequests;
      {
        std::unique_lock lock(mutex_);
        wake_.wait(lock, [this] { return stopping_ || !pending_.empty(); });
        if (stopping_) return;
        retiredRequests.swap(retiredRequests_);
        job.emplace(std::move(pending_.front())); pending_.pop_front();
        activeSlot_ = job->slot; activeKey_ = job->key;
        activeShared_ = job->shared; activeSuperseded_ = false;
        sweepRetired();
        // Only the owner evicts; leases held by render callers survive safely.
        // Reserve scratch + result space before running the producer.
        while (!results.empty() && (residentBytes_ + pendingBytes_ > budget_ ||
                                   results.size() + retired.size() >= residentCapacity_)) {
          if (!retireLeastUsed()) break;
          std::atomic_store(&published_, std::make_shared<const Results>(results));
          sweepRetired();
        }
        if (residentBytes_ + pendingBytes_ > budget_ || results.size() + retired.size() >= residentCapacity_) {
          pendingBytes_ -= job->maximumBytes; queued_.erase(job->key);
          refused_.fetch_add(1); job.reset(); activeSlot_.clear(); activeKey_.reset();
        } else highWaterBytes_ = std::max(highWaterBytes_, residentBytes_ + pendingBytes_);
        std::atomic_store(&published_, std::make_shared<const Results>(results));
      }
      retiredRequests.clear(); // release canceled requests off-lock on owner
      if (!job) continue;
      Result result;
      try { result = prepare_(job->request); }
      catch (...) { result.warning = "Graphics preparation failed."; }
      if (result.bytes > job->maximumBytes) {
        result.asset.reset(); result.bytes = 0; result.warning = "Graphics preparation exceeded its admitted budget.";
      }
      if (!result.asset) { result.bytes = 0; if (!result.retryable) failed_.fetch_add(1); }
      {
        std::lock_guard lock(mutex_);
        pendingBytes_ -= job->maximumBytes; queued_.erase(job->key);
        activeSlot_.clear(); activeKey_.reset();
        if (stopping_) return;
        // The in-flight CPU producer cannot be forcibly interrupted safely.
        // Its reservation stays charged until completion, and a superseded
        // result is destroyed here on the owner without being published.
        if (activeSuperseded_) { superseded_.fetch_add(1); continue; }
        // Temporary process-wide resource pressure is backpressure, not a
        // sticky missing-font/asset result. A later render may request it again.
        if (result.retryable && !result.asset) { refused_.fetch_add(1); continue; }
        residentBytes_ += result.bytes;
        results[job->key] = std::make_shared<Entry>(
            std::make_shared<const Result>(std::move(result)), ++useClock_);
        // Failed entries are bounded too, even though their GPU footprint is zero.
        while (results.size() > residentCapacity_) {
          if (!retireLeastUsed()) break;
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
  // Bounded separately by capacity_; canceled intents no longer reserve raster
  // scratch, but keep their bounded metadata/device lease until owner disposal.
  std::deque<Job> retiredRequests_;
  std::set<Key> queued_;
  bool stopping_ = false;
  std::string activeSlot_;
  std::optional<Key> activeKey_;
  bool activeSuperseded_ = false, activeShared_ = false;
  uint64_t residentBytes_ = 0, pendingBytes_ = 0, highWaterBytes_ = 0;
  std::atomic<uint64_t> admitted_{0}, completed_{0}, refused_{0}, failed_{0};
  std::atomic<uint64_t> superseded_{0};
  mutable std::atomic<uint64_t> useClock_{0};
  std::shared_ptr<const Results> published_;
  std::thread worker_;
};
}  // namespace corevideo::graphics
