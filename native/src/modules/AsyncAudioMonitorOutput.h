#pragma once
#include "modules/Interfaces.h"
#include <memory>
#include <thread>

namespace corevideo::modules {

// Device activation, render and stop belong to one device thread. DSP/control
// callers only submit bounded intent/PCM and read cached observations.
class AsyncAudioMonitorOutput final : public IAudioMonitorOutput {
 public:
  explicit AsyncAudioMonitorOutput(std::unique_ptr<IAudioMonitorOutput> inner);
  ~AsyncAudioMonitorOutput() override;
  bool start(const std::string&, int sampleRate, int channels) override;
  void stop() override;
  bool render(const float*, int frameCount, int channels, double volume) override;
  bool active() const override;
  bool hardwareOutput() const override;
  bool preparing() const override;
  std::string deviceName() const override;
  std::vector<std::string> warnings() const override;
  std::int64_t underrunCount() const override;
  std::string resolvedEndpointId() const override;
  std::int64_t acceptedFrameCount() const override;
  std::int64_t droppedFrameCount() const override;
  bool deviceDeliveryObserved() const override;
 private:
  struct State;
  std::shared_ptr<State> state_;
  std::thread worker_;
  static void run(const std::shared_ptr<State>&);
};

}  // namespace corevideo::modules
