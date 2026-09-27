#pragma once

#include "contracts/Lifecycle.h"
#include "core/ControlCommandPolicy.h"
#include "rpc/Json.h"

#include <cstdint>
#include <deque>
#include <map>
#include <optional>
#include <string>
#include <utility>

namespace corevideo::core {

// Serialized control-plane state for operator audio crosspoints. The core's
// existing matrix sync still owns bus topology; accepted crosspoint edits are
// replayed over that legacy sync until the editor domain is fully migrated.
class AudioRouteControlState {
 public:
  struct Override {
    bool enabled = false;
    double gainDb = 0;
  };
  using Key = std::pair<std::string, std::string>;

  explicit AudioRouteControlState(std::string epoch) : epoch_(std::move(epoch)) {}

  bool submit(const rpc::Json& command, bool validPayload) {
    const auto operationId = command.getString("operationId");
    const bool valid = contracts::validateControlOperationIdentity(command) && validPayload;
    const auto found = results_.find(operationId);
    const auto expected = valid ? static_cast<std::uint64_t>(command.getNumber("expectedRevision")) : 0;
    const auto decision = decideControlCommand(valid, found != results_.end(), epoch_, revision_,
                                                command.getString("authorityEpoch"), expected);
    if (decision == ControlCommandDecision::Duplicate) {
      lastResult_ = found->second;
      return false;
    }
    const bool applied = decision == ControlCommandDecision::Apply;
    if (applied) {
      overrides_[{command.getString("sourceId"), command.getString("busId")}] =
          {command.get("enabled")->asBool(), command.getNumber("gainDb", 0)};
      ++revision_;
    }
    lastResult_ = rpc::Json::Object{
        {"operationId", operationId},
        {"status", applied ? "applied" : decision == ControlCommandDecision::Conflict ? "conflict" : "invalid"},
        {"authorityEpoch", epoch_},
        {"revision", static_cast<double>(revision_)},
        {"expectedRevision", static_cast<double>(expected)},
    };
    if (valid) {
      results_[operationId] = lastResult_;
      order_.push_back(operationId);
      if (order_.size() > 32) {
        results_.erase(order_.front());
        order_.pop_front();
      }
    }
    return applied;
  }

  [[nodiscard]] const std::map<Key, Override>& overrides() const { return overrides_; }
  [[nodiscard]] rpc::Json snapshot() const {
    rpc::Json::Array recent;
    for (const auto& id : order_) recent.emplace_back(results_.at(id));
    return rpc::Json::Object{{"authorityEpoch", epoch_},
                             {"revision", static_cast<double>(revision_)},
                             {"lastResult", lastResult_}, {"recentResults", recent}};
  }

 private:
  std::string epoch_;
  std::uint64_t revision_ = 0;
  rpc::Json lastResult_;
  std::map<std::string, rpc::Json> results_;
  std::deque<std::string> order_;
  std::map<Key, Override> overrides_;
};

}  // namespace corevideo::core
