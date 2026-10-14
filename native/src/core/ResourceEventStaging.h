#pragma once
#include "rpc/Json.h"
#include "modules/Interfaces.h"
#include <array>
#include <algorithm>
#include <cstdint>

namespace corevideo::core {

// Caller owns synchronization. Admission precedes construction; accepted
// announcements are never evicted or coalesced. A drain transfers ownership,
// so final JSON/container retirement belongs to the optional consumer.
class ResourceEventStaging {
 public:
  enum class Kind { Program, Participant, Multiview, Preview };
  static constexpr std::size_t Capacity = 64, ByteBudget = 2 * 1024 * 1024;
  struct Facts { std::size_t items = 0, bytes = 0; std::uint64_t admitted = 0, refused = 0; };
  static bool valid(const modules::ProgramFrameSharedTexture& texture) {
    return (!texture.sharedHandleHex.empty() || texture.iosurfaceId != 0) && texture.width > 0 && texture.height > 0;
  }
  // Conservative resident allocation bounds for these fixed event schemas:
  // 8 KiB covers root/texture maps and fixed keys; each tile adds 4 KiB for
  // its map/keys. Eight times string length covers copied character allocations
  // and terminators/allocator rounding (and worst-case JSON escaping), in
  // addition to the fixed node allowance.
  static std::size_t bound(const modules::ProgramFrameSharedTexture& texture,
                           const std::vector<modules::MultiviewTileRect>* tiles = nullptr) {
    std::size_t bytes = 8192;
    add(bytes, texture.sharedHandleHex.size()); add(bytes, texture.format.size());
    if (tiles) for (const auto& tile : *tiles) {
      bytes = (std::min)(ByteBudget + 1, bytes + 4096);
      for (const auto* text : {&tile.sourceId, &tile.participantId, &tile.label, &tile.role, &tile.tally}) add(bytes, text->size());
    }
    return bytes;
  }
  static std::size_t bound(const modules::ParticipantSharedTexture& texture) {
    std::size_t bytes = 8192;
    for (const auto* text : {&texture.participantId, &texture.sharedHandleHex, &texture.format}) add(bytes, text->size());
    return bytes;
  }
  template<typename Build> bool admit(Kind kind, std::size_t ownedByteBound, Build&& build) {
    if (count_ == Capacity || ownedByteBound > ByteBudget || bytes_ > ByteBudget - ownedByteBound) {
      ++refused_; return false;
    }
    auto event = build();
    if (event.isNull()) return false;
    entries_[count_++] = {kind, ownedByteBound, std::move(event)};
    bytes_ += ownedByteBound; ++admitted_; return true;
  }
  std::vector<rpc::Json> drain() { return take(nullptr); }
  std::vector<rpc::Json> drain(Kind kind) { return take(&kind); }
  Facts facts() const { return {count_, bytes_, admitted_, refused_}; }
 private:
  static void add(std::size_t& bytes, std::size_t characters) {
    bytes = characters > ByteBudget / 8 ? ByteBudget + 1 : (std::min)(ByteBudget + 1, bytes + 8 * characters);
  }
  struct Entry { Kind kind{}; std::size_t bytes = 0; rpc::Json event; };
  std::vector<rpc::Json> take(const Kind* kind) {
    std::vector<rpc::Json> result; result.reserve(count_);
    std::size_t kept = 0;
    for (std::size_t i = 0; i < count_; ++i) {
      if (!kind || entries_[i].kind == *kind) {
        bytes_ -= entries_[i].bytes;
        result.push_back(std::move(entries_[i].event));
      } else {
        if (kept != i) entries_[kept] = std::move(entries_[i]);
        ++kept;
      }
    }
    count_ = kept; return result;
  }
  std::array<Entry, Capacity> entries_;
  std::size_t count_ = 0, bytes_ = 0;
  std::uint64_t admitted_ = 0, refused_ = 0;
};
}  // namespace corevideo::core
