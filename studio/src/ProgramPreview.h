#pragma once

#include <windows.h>

#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

class ProgramPreview {
 public:
  struct Metadata {
    int width = 0;
    int height = 0;
    std::uint64_t frameNumber = 0;
    std::string health;
    std::string renderer;
    std::string renderPlanId;
  };

  bool updateFromEventLine(std::string_view line);
  void clear();
  void setWaitingHint(std::string hint);

  bool hasFrame() const;
  std::optional<Metadata> latestMetadata() const;

  void render(HDC dc, const RECT& bounds) const;

 private:
  struct Frame {
    Metadata metadata;
    std::vector<std::uint8_t> bgra;
  };

  mutable std::mutex mutex_;
  std::optional<Frame> latest_;
  std::string waitingHint_ = "Waiting for program frames from native core.";
};
