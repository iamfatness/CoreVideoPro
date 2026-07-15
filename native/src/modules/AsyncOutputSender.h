#pragma once

#include "modules/Interfaces.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>

namespace corevideo::modules {

// Keeps network/FFmpeg I/O off the media-core thread. Only the freshest pending
// sync is retained; a slow sender drops stale frames instead of blocking render,
// pings, and the operator's stop command.
class AsyncOutputSender final : public IOutputSender {
 public:
  struct Options {
    std::chrono::milliseconds shutdownGrace{2000};
  };

  explicit AsyncOutputSender(std::unique_ptr<IOutputSender> inner);
  AsyncOutputSender(std::unique_ptr<IOutputSender> inner, Options options);
  ~AsyncOutputSender() override;

  OutputSenderSession sync(
      const std::vector<std::string>& destinations,
      const ProgramFrame* frame,
      double elapsedMs,
      const std::vector<OutputDestinationSettings>& destinationSettings = {},
      const std::vector<float>* programAudioPcm = nullptr,
      int audioChannels = 0,
      int audioSampleRate = 0) override;
  OutputSenderSession fail(const std::string& destination, const std::string& message, double elapsedMs) override;
  OutputSenderSession recover(const std::string& destination, double elapsedMs, const std::string& reason) override;
  OutputSenderSession session() const override;
  void interrupt(const std::string& destination) override;

  [[nodiscard]] uint64_t droppedSyncs() const;
  bool drainForTest(std::chrono::milliseconds timeout);

 private:
  enum class Kind { Sync, Fail, Recover };
  struct Item {
    Kind kind = Kind::Sync;
    uint64_t seq = 0;
    std::vector<std::string> destinations;
    std::unique_ptr<ProgramFrame> frame;
    double elapsedMs = 0;
    std::vector<OutputDestinationSettings> destinationSettings;
    std::vector<float> audioPcm;
    int audioChannels = 0;
    int audioSampleRate = 0;
    std::string destination;
    std::string message;
  };

  struct State {
    std::unique_ptr<IOutputSender> inner;
    std::mutex queueMutex;
    std::condition_variable queueCv;
    std::condition_variable appliedCv;
    std::deque<Item> queue;
    uint64_t nextSeq = 1;
    uint64_t appliedSeq = 0;
    bool stop = false;
    bool writerDone = false;
    std::atomic<uint64_t> dropped{0};
    std::mutex snapshotMutex;
    OutputSenderSession snapshot;
  };

  uint64_t enqueue(Item&& item);
  static void writerLoop(std::shared_ptr<State> state);

  Options options_;
  std::shared_ptr<State> state_;
  std::thread writer_;
};

}  // namespace corevideo::modules
