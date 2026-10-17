#pragma once
#include <windows.h>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <string_view>

namespace corevideo::virtualcam {
// DLL-static lifetime in production. The queued callback retains the DLL until
// Windows returns from it, including when COM has released its last source.
class VcamLogQueue {
 public:
  static constexpr size_t Capacity = 128, MessageBytes = 2048;
  using Sink = bool (*)(std::string_view, void*);
  VcamLogQueue(Sink sink, void* context) : sink_(sink), context_(context) {
    work_ = CreateThreadpoolWork(&Drain, this, nullptr);
  }
  ~VcamLogQueue() { if (work_) CloseThreadpoolWork(work_); }
  VcamLogQueue(const VcamLogQueue&) = delete;
  VcamLogQueue& operator=(const VcamLogQueue&) = delete;
  void write(std::string_view message) noexcept {
    std::unique_lock<std::mutex> lock(mutex_, std::try_to_lock);
    if (!lock.owns_lock() || !work_ || size_ == Capacity) { ++dropped_; return; }
    auto& entry = queue_[(head_ + size_) % Capacity];
    entry.size = message.size() < MessageBytes ? message.size() : MessageBytes - 1;
    std::memcpy(entry.bytes.data(), message.data(), entry.size);
    if (entry.size != message.size()) ++truncated_;
    ++size_;
    if (scheduled_) return;
    // Hold a real module reference BEFORE submitting, so a queued callback is
    // safe too. FreeLibraryWhenCallbackReturns releases it after our code ends.
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
          reinterpret_cast<LPCWSTR>(&Drain), &module_)) {
      dropped_ += size_; size_ = 0; return;
    }
    scheduled_ = true;
    SubmitThreadpoolWork(work_);
  }
  uint64_t dropped() const { return dropped_.load(); }
  uint64_t truncated() const { return truncated_.load(); }
  uint64_t failures() const { return failures_.load(); }
  // Only non-media test/owner teardown may wait, after unblocking the sink.
  void waitForCallbacks() { if (work_) WaitForThreadpoolWorkCallbacks(work_, FALSE); }
 private:
  struct Message { std::array<char, MessageBytes> bytes{}; size_t size = 0; };
  static void CALLBACK Drain(PTP_CALLBACK_INSTANCE instance, void* context, PTP_WORK) noexcept {
    auto& self = *static_cast<VcamLogQueue*>(context);
    HMODULE module;
    { std::lock_guard<std::mutex> lock(self.mutex_); module = self.module_; }
    FreeLibraryWhenCallbackReturns(instance, module);
    CallbackMayRunLong(instance);
    for (;;) {
      Message message;
      {
        std::lock_guard<std::mutex> lock(self.mutex_);
        if (!self.size_) { self.scheduled_ = false; return; }
        message = self.queue_[self.head_];
        self.head_ = (self.head_ + 1) % Capacity; --self.size_;
      }
      try {
        if (!self.sink_(std::string_view(message.bytes.data(), message.size), self.context_)) ++self.failures_;
      } catch (...) { ++self.failures_; }
    }
  }
  std::mutex mutex_;
  std::array<Message, Capacity> queue_{};
  size_t head_ = 0, size_ = 0;
  bool scheduled_ = false;
  HMODULE module_ = nullptr;
  PTP_WORK work_ = nullptr;
  Sink sink_;
  void* context_;
  std::atomic<uint64_t> dropped_{0}, truncated_{0}, failures_{0};
};
}
