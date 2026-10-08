#pragma once
#include "rpc/Json.h"
#include <cstdint>
#include <cstddef>
#include <memory>
#include <functional>
#include <string>
#include <string_view>

namespace corevideo::core {
// Only these boundaries are observed in v1. Camera/monitor/shell remain unknown.
enum class DeliveryStage : std::uint32_t {
  SourceGpuReady = 1, SourceAdmitted, ProgramSubmitted, ProgramGpuReady,
  ProgramDelivered, ProgramMiss, SourceRequested, SourceUploadStarted, SourceUploadSubmitted, SourceUploadRefused
};
enum class DeliveryReason : std::uint32_t { None, Ready, Held, Unavailable, Failed };
struct DeliveryTraceEvent {
  std::uint64_t sessionEpoch = 0, sourceTag = 0, sourceEpoch = 0;
  std::int64_t programSequence = -1, sourceFrameId = -1, timestamp = 0;
  // Source observation time, NOT verified acquisition/exposure time. Separate
  // steady-clock 100ns domain; never subtract it from QPC without calibration.
  std::int64_t sourceObservation100ns = 0;
  std::uint64_t layoutTag = 0;
  DeliveryStage stage = DeliveryStage::ProgramSubmitted;
  DeliveryReason reason = DeliveryReason::None;
};
static_assert(sizeof(DeliveryTraceEvent) == 72);
struct DeliveryTraceHeader {
  char magic[8] = {'C','V','T','R','A','C','E','1'};
  std::uint32_t version = 1, eventBytes = sizeof(DeliveryTraceEvent);
  std::uint64_t clockFrequency = 0, sessionEpoch = 0, exported = 0, lost = 0;
  std::uint64_t failures = 0, complete = 0;
  std::int64_t started = 0, ended = 0;
};
static_assert(sizeof(DeliveryTraceHeader) == 80);
class DeliveryTraceCapture {
 public:
  static constexpr std::size_t kStorageBytes = 16 * 1024 * 1024;
  // Constructor-only fault seam; no operator command/environment binding.
  explicit DeliveryTraceCapture(const std::string& path, std::function<void()> beforeExport = {});
  ~DeliveryTraceCapture();
  DeliveryTraceCapture(const DeliveryTraceCapture&) = delete;
  DeliveryTraceCapture& operator=(const DeliveryTraceCapture&) = delete;
  // At most sixteen head/slot attempts: contention/full storage drops diagnostics only.
  bool record(DeliveryTraceEvent event) noexcept;
  bool close(); // At most two seconds; a blocked sink retains its owned state.
  std::uint64_t tag(std::string_view identity) const noexcept;
  rpc::Json snapshot() const;
 private:
  struct State;
  std::shared_ptr<State> state_;
};
// Called once before media workers and kept alive until after their shutdown.
// Explicit path enables capture; unset means no allocation/thread/clock work.
std::unique_ptr<DeliveryTraceCapture> startDeliveryTraceFromEnvironment();
void recordDeliveryTrace(DeliveryTraceEvent event) noexcept;
rpc::Json deliveryEvidenceSnapshot();
std::uint64_t deliveryTraceTag(std::string_view identity) noexcept;
std::int64_t deliveryTraceNow() noexcept;
}
