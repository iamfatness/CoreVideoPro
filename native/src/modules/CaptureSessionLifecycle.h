#pragma once
#include "core/ComApartmentLifetime.h"
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <utility>

namespace corevideo::modules {
// A bounded command mailbox. Creation and stop/join belong to this owner, never
// to a poll/render caller. Shared read leases may outlive retirement. Changes coalesce
// to its newest request; a superseded start cannot publish a stale session.
template<class Session> class CaptureSessionLifecycle {
 public:
  using Create = std::function<std::shared_ptr<Session>(const std::string&)>;
  using Retire = std::function<void(Session&)>;
  explicit CaptureSessionLifecycle(Create create, Retire retire, size_t capacity = 16)
      : create_(std::move(create)), retire_(std::move(retire)), capacity_(capacity),
        thread_([this] { run(); }) {}
  ~CaptureSessionLifecycle() {
    { std::lock_guard<std::mutex> lock(mutex_); stopping_ = true; pending_.clear(); }
    ready_.notify_one();
    if (thread_.joinable()) thread_.join(); // final process/module shutdown only
  }
  bool connect(const std::string& id) { return request(id, true); }
  bool disconnect(const std::string& id) { return request(id, false); }
  std::map<std::string, std::shared_ptr<Session>> snapshot() const {
    std::map<std::string, std::shared_ptr<Session>> result;
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& [id, session] : active_) {
      const auto requested = requests_.find(id);
      if (requested != requests_.end() && requested->second.live) result.emplace(id, session);
    }
    return result;
  }
  std::string status(const std::string& id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto request = requests_.find(id);
    if (request == requests_.end()) return "detected";
    if (pending_.count(id) || inFlight_ == id) return request->second.live ? "connecting" : "stopping";
    if (failed_.count(id)) return "failed";
    return active_.count(id) ? "connected" : "detected";
  }
 private:
  struct Request { uint64_t revision = 0; bool live = false; };
  bool request(const std::string& id, bool live) {
    if (id.empty()) return false;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (stopping_) return false;
      if (!live && !requests_.count(id)) return true;
      if (!requests_.count(id) && requests_.size() >= capacity_) return false;
      requests_[id] = {++revision_, live}; pending_.insert(id); failed_.erase(id);
    }
    ready_.notify_one(); return true;
  }
  void retire(std::shared_ptr<Session> session) noexcept {
    if (session) { try { retire_(*session); } catch (...) {} }
    // Stop/join has completed before a snapshot can release its final reference.
  }
  void run() {
    core::ComApartmentLifetime apartment;
    for (;;) {
      std::string id; Request request; std::shared_ptr<Session> old;
      {
        std::unique_lock<std::mutex> lock(mutex_);
        ready_.wait(lock, [this] { return stopping_ || !pending_.empty(); });
        if (stopping_) break;
        id = *pending_.begin(); pending_.erase(pending_.begin());
        request = requests_.at(id); inFlight_ = id;
        const auto current = active_.find(id);
        if (current != active_.end()) { old = std::move(current->second); active_.erase(current); }
      }
      retire(std::move(old));
      bool current = false;
      {
        std::lock_guard<std::mutex> lock(mutex_);
        current = !stopping_ && requests_.at(id).revision == request.revision;
      }
      std::shared_ptr<Session> next;
      if (current && request.live) { try { next = create_(id); } catch (...) {} }
      {
        std::lock_guard<std::mutex> lock(mutex_);
        current = !stopping_ && requests_.at(id).revision == request.revision;
        if (current) {
          if (next) active_.emplace(id, std::move(next));
          else if (request.live) failed_.insert(id);
          else requests_.erase(id);
        }
        inFlight_.clear();
      }
      retire(std::move(next)); // cancelled creation cannot enter the poll roster
    }
    std::map<std::string, std::shared_ptr<Session>> remaining;
    { std::lock_guard<std::mutex> lock(mutex_); remaining.swap(active_); }
    for (auto& [id, session] : remaining) retire(std::move(session));
  }
  Create create_; Retire retire_; size_t capacity_;
  mutable std::mutex mutex_; std::condition_variable ready_;
  std::map<std::string, Request> requests_;
  std::map<std::string, std::shared_ptr<Session>> active_;
  std::set<std::string> pending_, failed_; std::string inFlight_;
  uint64_t revision_ = 0; bool stopping_ = false; std::thread thread_;
};
}
