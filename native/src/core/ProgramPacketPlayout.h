#pragma once
#include "core/DeliveredProgramPacket.h"
#include "core/ProgramPlayoutTimeline.h"
#include <map>
#include <mutex>
namespace corevideo::core {
class IProgramPlayout {
 public:
  enum class Admission { Accepted, Invalid, StaleClock, Expired, Conflict, Capacity, Stopped };
  enum class Status { Delivered, LateDelivery, Missing, GpuNotReady, GpuLate, SequenceExhausted };
  struct Decision {
    Status status; int64_t slot, deadlineNs, skippedSlots;
    std::shared_ptr<const DeliveredProgramPacket> packet;
  };
  virtual ~IProgramPlayout()=default;
  virtual Admission enqueue(DeliveredProgramPacket)=0;
  virtual std::optional<Decision> takeDue(int64_t nowNs)=0;
};
class ProgramPacketPlayout final : public IProgramPlayout {
 public:
  struct Config { int depth{3}; std::string clockEpoch; uint64_t clockGeneration{1}; int64_t anchorNs{0}, initialSlot{0}; size_t maxBytes{128*1024*1024}; uint64_t initialDeliverySequence{0}; };
  explicit ProgramPacketPlayout(Config);
  Admission enqueue(DeliveredProgramPacket) override;
  // Full packet key; stale GPU callbacks cannot make a replacement frame ready.
  Admission gpuReady(const std::string& epoch,uint64_t generation,int64_t slot,
      uint64_t packetSequence,uint64_t renderRevision,int64_t observedAtNs);
  std::optional<Decision> takeDue(int64_t nowNs) override;
  // Transfers retirement ownership; caller may release GPU leases on its proper
  // asynchronous resource owner. No deleters run under this adapter's lock.
  std::vector<std::shared_ptr<const DeliveredProgramPacket>> stop();
 private:
  struct Entry { std::shared_ptr<const DeliveredProgramPacket> packet; size_t bytes; std::optional<int64_t> gpuReadyAt; };
  const Config config_;
  ProgramPlayoutTimeline timeline_;
  std::mutex mutex_;
  std::map<int64_t,Entry> queue_;
  size_t bytes_{0}; uint64_t deliverySequence_{0}; bool stopped_{false};
};
}
