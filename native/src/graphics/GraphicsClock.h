#pragma once
#include "rpc/Json.h"
#include <atomic>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>
#include <optional>

namespace corevideo::graphics {
struct GraphicsClockConfig {
  std::string mode="wall", zone="local", format="hms", finish="hold-zero";
  int offsetMinutes=0, durationSeconds=300;
  std::optional<double> restoreRemainingSeconds;
  static bool read(const rpc::Json&,GraphicsClockConfig&);
  rpc::Json json() const;
};
// Pure clock state: wall time supplies content; a monotonic anchor supplies
// countdown progress. No renderer, audio or implicit Show/Take side effects.
class GraphicsClock {
 public:
  GraphicsClockConfig config;
  void control(const std::string& action,double monotonicMs,const GraphicsClockConfig* replacement=nullptr);
  rpc::Json sample(double monotonicMs,int64_t wallSeconds) const;
 private:
  bool running_=false;
  double anchorMs_=0,remainingSeconds_=300;
};
// One bounded controller per clock instance (the graphics instance limit is 8).
// Formatting runs here, never in the compositor or its preparation workers.
class GraphicsClockController {
 public:
  explicit GraphicsClockController(GraphicsClockConfig initial = {});
  ~GraphicsClockController();
  bool submit(const std::string& action,const GraphicsClockConfig& config);
  std::shared_ptr<const rpc::Json> sample() const { return std::atomic_load(&sample_); }
 private:
  void run(GraphicsClockConfig);
  struct Command { std::string action; GraphicsClockConfig config; };
  std::mutex mutex_; std::condition_variable wake_; std::deque<Command> commands_;
  bool stopping_=false;
  std::shared_ptr<const rpc::Json> sample_;
  // The worker publishes sample_; initialize its storage before starting it.
  std::thread worker_;
};
}
