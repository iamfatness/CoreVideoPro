#pragma once
#include "core/CoreStateCapture.h"
#include <condition_variable>
#include <thread>

namespace corevideo::rpc {
// One executing and one pending capture. Capacity waits and response retirement
// happen outside coreMutex. Accepted command responses are never coalesced.
class ResponseAssemblyLane {
 public:
  using Assemble = std::function<Json(const core::CoreStateCapture&, core::SnapshotProjection)>;
  using Deliver = std::function<void(std::string)>;
  struct Job {
    Json response;
    std::unique_ptr<core::CoreStateCapture> capture;
    core::SnapshotProjection kind = core::SnapshotProjection::Session;
    std::string field;
  };
  ResponseAssemblyLane(Assemble assemble, Deliver deliver)
      : assemble_(std::move(assemble)), deliver_(std::move(deliver)), worker_([this] { run(); }) {}
  ~ResponseAssemblyLane() { finish(); }
  void waitForCapacity() {
    std::unique_lock lock(mutex_);
    ready_.wait(lock, [this] { return !pending_; });
  }
  // Single producer, after waitForCapacity and after releasing coreMutex.
  void submit(Job job) {
    { std::lock_guard lock(mutex_); pending_ = std::make_unique<Job>(std::move(job)); }
    ready_.notify_all();
  }
  void finish() {
    { std::lock_guard lock(mutex_); finishing_ = true; }
    ready_.notify_all();
    if (worker_.joinable()) worker_.join();
  }
 private:
  void run() {
    for (;;) {
      std::unique_ptr<Job> job;
      {
        std::unique_lock lock(mutex_);
        ready_.wait(lock, [this] { return pending_ || finishing_; });
        if (!pending_) return;
        job = std::move(pending_);
      }
      ready_.notify_all();
      try {
        auto projected = assemble_ ? assemble_(*job->capture, job->kind) : job->capture->assemble(job->kind);
        auto object = job->response.asObject();
        object.insert_or_assign(job->field, std::move(projected));
        deliver_(Json(std::move(object)).stringify());
      } catch (const std::exception& e) {
        deliver_(Json(Json::Object{{"id", *job->response.get("id")}, {"ok", false},
          {"error", Json::Object{{"code", "response-assembly-failed"}, {"message", e.what()}}}}).stringify());
      }
      // Captured metadata and observations retire here, never under coreMutex.
    }
  }
  Assemble assemble_;
  Deliver deliver_;
  std::mutex mutex_;
  std::condition_variable ready_;
  std::unique_ptr<Job> pending_;
  bool finishing_ = false;
  std::thread worker_;
};
}
