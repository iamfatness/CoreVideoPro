#include "core/ProgramPacketPlayout.h"
#include <stdexcept>
namespace corevideo::core {
namespace {
ProgramPacketPlayout::Config checked(ProgramPacketPlayout::Config c) {
  // Conservative slot bound keeps timeline arithmetic below max safe nanoseconds.
  constexpr auto maxSlot=(kProgramPacketMaxInteger/1000000000LL-1)*60;
  if((c.depth!=2&&c.depth!=3)||c.clockEpoch.empty()||c.clockEpoch.size()>512||!c.clockGeneration||
      c.clockGeneration>kProgramPacketMaxInteger||c.anchorNs<0||c.anchorNs>kProgramPacketMaxInteger||
      c.initialDeliverySequence>kProgramPacketMaxInteger||c.initialSlot<0||c.initialSlot>maxSlot||!c.maxBytes||c.maxBytes>256*1024*1024)
    throw std::invalid_argument("Invalid Program playout configuration");
  ProgramPlayoutTimeline t(c.depth,0,c.initialSlot);
  if(t.nextDeadlineNs()>kProgramPacketMaxInteger-c.anchorNs)throw std::invalid_argument("Program clock exhausted");
  return c;
}
}
ProgramPacketPlayout::ProgramPacketPlayout(Config c):config_(checked(std::move(c))),timeline_(config_.depth,config_.anchorNs,config_.initialSlot),deliverySequence_(config_.initialDeliverySequence){}
IProgramPlayout::Admission ProgramPacketPlayout::enqueue(DeliveredProgramPacket p) {
  const auto validation=validateDeliveredProgramPacket(p);
  if(!validation.valid||p.delivery||p.packetSequence!=static_cast<uint64_t>(p.productionSlot)+1)return Admission::Invalid;
  if(p.producedAtNs>p.deliveryDeadlineNs)return Admission::Expired;
  if(p.clockEpoch!=config_.clockEpoch||p.clockGeneration!=config_.clockGeneration)return Admission::StaleClock;
  constexpr auto maxSlot=(kProgramPacketMaxInteger/1000000000LL-1)*60;
  if(p.productionSlot>maxSlot)return Admission::Invalid;
  ProgramPlayoutTimeline zero(config_.depth,0);
  const auto offset=zero.deadlineNs(p.productionSlot);
  if(offset>kProgramPacketMaxInteger-config_.anchorNs ||
      p.deliveryDeadlineNs!=config_.anchorNs+offset ||
      p.productionDeadlineNs!=config_.anchorNs+ProgramPlayoutTimeline(2,0).deadlineNs(p.productionSlot-1) ||
      p.producedAtNs<config_.anchorNs)return Admission::Invalid;
  auto frozen=std::make_shared<const DeliveredProgramPacket>(std::move(p));
  std::lock_guard lock(mutex_);
  if(stopped_)return Admission::Stopped;
  if(timeline_.isExpired(frozen->productionSlot))return Admission::Expired;
  if(queue_.contains(frozen->productionSlot))return Admission::Conflict;
  if(queue_.size()>=static_cast<size_t>(config_.depth)||validation.retainedBytes>config_.maxBytes-bytes_)return Admission::Capacity;
  queue_.emplace(frozen->productionSlot,Entry{frozen,validation.retainedBytes,{}});bytes_+=validation.retainedBytes;
  return Admission::Accepted;
}
IProgramPlayout::Admission ProgramPacketPlayout::gpuReady(const std::string& epoch,uint64_t generation,int64_t slot,
    uint64_t sequence,uint64_t revision,int64_t at) {
  if(epoch!=config_.clockEpoch||generation!=config_.clockGeneration)return Admission::StaleClock;
  if(at<config_.anchorNs||at>kProgramPacketMaxInteger)return Admission::Invalid;
  std::lock_guard lock(mutex_);
  if(stopped_)return Admission::Stopped;
  if(timeline_.isExpired(slot))return Admission::Expired;
  const auto found=queue_.find(slot);
  if(found==queue_.end()||found->second.packet->packetSequence!=sequence||found->second.packet->renderRevision!=revision)return Admission::Conflict;
  if(!found->second.packet->gpuLease||at<found->second.packet->producedAtNs)return Admission::Invalid;
  if(found->second.gpuReadyAt)return *found->second.gpuReadyAt==at?Admission::Accepted:Admission::Conflict;
  found->second.gpuReadyAt=at;return Admission::Accepted;
}
std::optional<IProgramPlayout::Decision> ProgramPacketPlayout::takeDue(int64_t now) {
  if(now<0||now>kProgramPacketMaxInteger)return {};
  std::map<int64_t,Entry> retired;
  std::lock_guard lock(mutex_);
  if(stopped_)return {};
  auto stagedTimeline=timeline_;
  const auto due=stagedTimeline.takeDue(now);if(!due)return {};
  Decision result{Status::Missing,due->slot,due->deadlineNs,due->skippedSlots,{}};
  std::shared_ptr<DeliveredProgramPacket> delivery;
  const auto candidate=queue_.find(due->slot);
  if(deliverySequence_<kProgramPacketMaxInteger && candidate!=queue_.end() && (!candidate->second.packet->gpuLease ||
      (candidate->second.gpuReadyAt && *candidate->second.gpuReadyAt<=due->deadlineNs))) {
    delivery=std::make_shared<DeliveredProgramPacket>(*candidate->second.packet);
    delivery->delivery=DeliveredProgramPacket::Delivery{deliverySequence_+1,now};
  }
  // Allocate before consuming the due decision: allocation failure is retryable.
  (void)timeline_.takeDue(now);
  for(auto it=queue_.begin();it!=queue_.end()&&it->first<=due->slot;) {
    if(it->first==due->slot) {
      const auto& e=it->second;
      if(e.packet->gpuLease&&!e.gpuReadyAt)result.status=Status::GpuNotReady;
      else if(e.packet->gpuLease&&*e.gpuReadyAt>due->deadlineNs)result.status=Status::GpuLate;
      else if(deliverySequence_==kProgramPacketMaxInteger)result.status=Status::SequenceExhausted;
      else {
        ++deliverySequence_;
        result.packet=std::move(delivery);
        result.status=now==due->deadlineNs?Status::Delivered:Status::LateDelivery;
      }
    }
    bytes_-=it->second.bytes;retired.insert(queue_.extract(it++));
  }
  return result;
}
std::vector<std::shared_ptr<const DeliveredProgramPacket>> ProgramPacketPlayout::stop() {
  std::vector<std::shared_ptr<const DeliveredProgramPacket>> leases;
  std::lock_guard lock(mutex_);
  leases.reserve(queue_.size());
  for(auto& [slot,e]:queue_)leases.push_back(std::move(e.packet));
  queue_.clear();bytes_=0;stopped_=true;return leases;
}
}
