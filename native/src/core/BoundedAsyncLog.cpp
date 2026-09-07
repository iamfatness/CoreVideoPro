#include "core/BoundedAsyncLog.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <thread>
#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <cerrno>
#include <csignal>
#include <pthread.h>
#include <unistd.h>
#endif

namespace corevideo::core {
struct BoundedAsyncLog::State {
  struct Message { std::array<char, kMessageBytes> bytes{}; std::size_t size = 0; };
  explicit State(Sink callback) : sink(std::move(callback)) {}
  std::mutex mutex;
  std::condition_variable changed;
  std::array<Message, kCapacity> queue;
  std::size_t head = 0, size = 0;
  bool stopped = false;
  Sink sink;
  std::atomic<std::uint64_t> dropped{0}, truncated{0}, sinkFailures{0};
};

BoundedAsyncLog::BoundedAsyncLog(Sink sink) : state_(std::make_shared<State>(std::move(sink))) {
  std::thread([state = state_] {
#if !defined(_WIN32)
    // A closed diagnostic pipe is a sink failure, not permission to terminate
    // the media process. Mask on this dedicated worker only; no global handler.
    sigset_t signals;
    sigemptyset(&signals);
    sigaddset(&signals, SIGPIPE);
    pthread_sigmask(SIG_BLOCK, &signals, nullptr);
#endif
    for (;;) {
      State::Message message;
      {
        std::unique_lock<std::mutex> lock(state->mutex);
        state->changed.wait(lock, [&] { return state->stopped || state->size != 0; });
        if (state->stopped) return;
        message = state->queue[state->head];
        state->head = (state->head + 1) % kCapacity;
        --state->size;
      }
      // The only blocking operation runs without the queue mutex. A blocked
      // sink retains at most this worker, its fixed queue, and one message.
      try {
        if (!state->sink(std::string_view(message.bytes.data(), message.size))) ++state->sinkFailures;
      } catch (...) { ++state->sinkFailures; }
    }
  }).detach();
}

BoundedAsyncLog::~BoundedAsyncLog() {
  {
    std::lock_guard<std::mutex> lock(state_->mutex);
    state_->stopped = true;
    state_->dropped.fetch_add(state_->size);
    state_->size = 0;
  }
  state_->changed.notify_one();
  // No join: even shutdown must not wait for a parent that stopped draining.
}

void BoundedAsyncLog::write(std::string_view message) noexcept {
  std::unique_lock<std::mutex> lock(state_->mutex, std::try_to_lock);
  if (!lock.owns_lock()) { ++state_->dropped; return; }
  if (state_->stopped || state_->size == kCapacity) { ++state_->dropped; return; }
  auto& entry = state_->queue[(state_->head + state_->size) % kCapacity];
  entry.size = (std::min)(message.size(), kMessageBytes - 1);
  std::memcpy(entry.bytes.data(), message.data(), entry.size);
  if (message.size() > entry.size) {
    ++state_->truncated;
    entry.bytes[entry.size - 1] = '\n';
  }
  ++state_->size;
  lock.unlock();
  state_->changed.notify_one();
}

BoundedAsyncLog::Stats BoundedAsyncLog::stats() const {
  std::lock_guard<std::mutex> lock(state_->mutex);
  return {state_->dropped.load(), state_->truncated.load(), state_->sinkFailures.load(), state_->size};
}

namespace {
BoundedAsyncLog& processLog() {
  // Intentionally process lifetime: do not enter CRT teardown with a worker
  // writing through a FILE lock. OS stderr writes also avoid that shared lock.
  static auto* logger = new BoundedAsyncLog([](std::string_view message) {
#if defined(_WIN32)
    DWORD written = 0;
    return WriteFile(GetStdHandle(STD_ERROR_HANDLE), message.data(),
        static_cast<DWORD>(message.size()), &written, nullptr) && written == message.size();
#else
    std::size_t offset = 0;
    while (offset < message.size()) {
      const auto count = ::write(STDERR_FILENO, message.data() + offset, message.size() - offset);
      if (count < 0 && errno == EINTR) continue;
      if (count <= 0) return false;
      offset += static_cast<std::size_t>(count);
    }
    return true;
#endif
  });
  return *logger;
}
}

void nativeLogf(const char* format, ...) noexcept {
  char message[BoundedAsyncLog::kMessageBytes];
  va_list args;
  va_start(args, format);
  const int length = std::vsnprintf(message, sizeof(message), format, args);
  va_end(args);
  if (length < 0) return;
  try {
    // Preserve the truncation signal without constructing a larger buffer.
    const auto size = (std::min)(static_cast<std::size_t>(length), sizeof(message));
    processLog().write(std::string_view(message, size));
  } catch (...) { /* Logging must not unwind media code on allocation/startup failure. */ }
}

BoundedAsyncLog::Stats nativeLogStats() { return processLog().stats(); }
}  // namespace corevideo::core
