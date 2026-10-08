#pragma once
#include "core/DeliveryTrace.h"
#include <array>
#include <cmath>

namespace corevideo::core {
// Exporter-owned numeric statistics. Inter-event intervals combine all sources
// at one stage; they are not source cadence or end-to-end content latency.
class DeliveryTraceAggregate {
 public:
  static constexpr std::array<double, 19> kUpperBoundsMs{
      .25, .5, 1, 2, 4, 8, 16, 16.667, 33.4, 66.8, 125, 250, 500, 1000, 2000, 4000, 8000, 16000, 60000};
  void observe(const DeliveryTraceEvent& event, std::uint64_t frequency) noexcept {
    const auto index = static_cast<std::uint32_t>(event.stage);
    if (index < 1 || index > rows_.size() || !frequency) return;
    auto& row = rows_[index - 1];
    ++row.count;
    const auto reason = static_cast<std::uint32_t>(event.reason);
    if (reason < row.reasons.size()) ++row.reasons[reason];
    if (row.observed && event.timestamp < row.last.timestamp) {
      ++row.outOfOrder;
      return; // Cross-thread export order need not be timestamp order.
    }
    if (row.observed) {
      const double interval = static_cast<double>(event.timestamp - row.last.timestamp) * 1000 / frequency;
      std::size_t bucket = 0;
      while (bucket < kUpperBoundsMs.size() && interval > kUpperBoundsMs[bucket]) ++bucket;
      ++row.intervals[bucket];
    }
    row.last = event; row.observed = true;
  }
  rpc::Json snapshot(std::int64_t at, std::uint64_t frequency, std::uint64_t revision) const {
    rpc::Json::Array bounds;
    for (auto value : kUpperBoundsMs) bounds.emplace_back(static_cast<double>(std::llround(value * 1000)));
    rpc::Json::Array rows;
    for (std::size_t index = 0; index < rows_.size(); ++index) {
      const auto& row = rows_[index];
      rpc::Json::Array intervals, reasons;
      for (auto value : row.intervals) intervals.emplace_back(static_cast<double>(value));
      for (auto value : row.reasons) reasons.emplace_back(static_cast<double>(value));
      rpc::Json::Object result{{"stage", static_cast<int>(index + 1)}, {"observed", row.observed},
          {"exportedEvents", static_cast<double>(row.count)}, {"timestampOrderInversions", static_cast<double>(row.outOfOrder)},
          {"reasonCounts", std::move(reasons)}, {"intervalCounts", std::move(intervals)}};
      if (row.observed) {
        result.emplace("lastTicks", std::to_string(row.last.timestamp));
        result.emplace("lastSourceTag", std::to_string(row.last.sourceTag));
        result.emplace("lastSourceEpoch", std::to_string(row.last.sourceEpoch));
        result.emplace("lastProgramSequence", std::to_string(row.last.programSequence));
        result.emplace("lastSourceFrameId", std::to_string(row.last.sourceFrameId));
        result.emplace("lastLayoutTag", std::to_string(row.last.layoutTag));
        if (at >= row.last.timestamp && frequency)
          result.emplace("lastProgressAgeTicks", std::to_string(at - row.last.timestamp));
      }
      rows.emplace_back(std::move(result));
    }
    return rpc::Json::Object{{"schemaVersion", "delivery-aggregate-v1"}, {"revision", static_cast<double>(revision)},
        {"observedAtTicks", std::to_string(at)}, {"refreshPeriodMs", 1000},
        {"scope", "Exported stage events; combined-source inter-event intervals, not per-source cadence, GPU duration, acquisition or receiver latency."},
        {"intervalUpperBoundsUs", std::move(bounds)}, {"stages", std::move(rows)}};
  }
 private:
  struct Row {
    bool observed = false;
    std::uint64_t count = 0, outOfOrder = 0;
    std::array<std::uint64_t, 5> reasons{};
    std::array<std::uint64_t, kUpperBoundsMs.size() + 1> intervals{};
    DeliveryTraceEvent last;
  };
  std::array<Row, 10> rows_;
};
}
