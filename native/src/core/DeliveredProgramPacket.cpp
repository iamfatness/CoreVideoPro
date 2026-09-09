#include "core/DeliveredProgramPacket.h"
#include <set>
#include <limits>
namespace corevideo::core {
ProgramPacketValidation validateDeliveredProgramPacket(const DeliveredProgramPacket& p) {
  const auto text=[](const std::string& s){return !s.empty() && s.size()<=512;};
  const auto positive=[](uint64_t n){return n>0 && n<=kProgramPacketMaxInteger;};
  if (!validExactRouteSourceRef({p.showEpoch,p.sceneId,p.clockEpoch,"camera",1}) || !text(p.showEpoch)||!text(p.sceneId)||!text(p.clockEpoch)||p.showRevision>kProgramPacketMaxInteger||
      !positive(p.sceneRevision)||!positive(p.renderRevision)||!positive(p.clockGeneration)||!positive(p.packetSequence)||
      p.productionSlot<0||p.productionSlot>kProgramPacketMaxInteger||
      p.packetSequence!=static_cast<uint64_t>(p.productionSlot)+1||p.producedAtNs<0||
      p.productionDeadlineNs<0||p.deliveryDeadlineNs<p.productionDeadlineNs||
      p.producedAtNs>kProgramPacketMaxInteger||p.deliveryDeadlineNs>kProgramPacketMaxInteger||
      p.sources.size()>4096||p.width<=0||p.height<=0||p.width>16384||p.height>16384||
      (!p.pixels&&!p.gpuLease)||(!p.pixels&&p.stride!=0)||(bool(p.gpuLease)!=(p.gpuBytes>0))) return {};
  size_t bytes=sizeof(p)+p.showEpoch.capacity()+p.sceneId.capacity()+p.clockEpoch.capacity();
  const auto add=[&](size_t n){if(n>256*1024*1024 || bytes>256*1024*1024-n)return false;bytes+=n;return true;};
  if(p.sources.capacity()>4096 || !add(p.sources.capacity()*sizeof(ExactRouteSourceRef)))return {};
  std::set<std::string> ids;
  for(const auto& s:p.sources) {
    if(!validExactRouteSourceRef(s)||!ids.insert(s.sourceId).second||
       !add(s.sourceId.capacity())||!add(s.instanceId.capacity())||!add(s.processEpoch.capacity())||!add(s.kind.capacity()))return {};
  }
  if(p.pixels && (p.stride<p.width*4 || p.stride>65536 ||
      p.pixels->size()<static_cast<size_t>(p.stride)*p.height || !add(p.pixels->capacity())))return {};
  if(!add(p.gpuBytes))return {};
  if(p.delivery && (!positive(p.delivery->sequence)||p.delivery->observedAtNs<p.deliveryDeadlineNs||
      p.delivery->observedAtNs>kProgramPacketMaxInteger))return {};
  return {true,bytes};
}
}
