#pragma once

// Comms (Zoom talkback) in the media core: the shell's comms-* requests
// checked and turned into the engine's comms_* lines, and the engine's
// comms_state turned into the camelCase `comms` object the core pushes to the
// shell (docs/reference/comms-talkback.md). Ported verbatim from CoreVideo
// Edge's native/src/edge/CommsRelay.{h,cpp} (the "node" there is the core
// here); keep the two in step. Pure: no engine, no process, so tests drive it.
//
// The core refuses what it can see is wrong (bad arguments, not in a meeting,
// a key while Comms is unavailable, assigning a web-client participant) with
// operator text; everything else is the engine's call and comes back in the
// state (ops log, lamps, cells). Releasing a key is never refused.

#include "rpc/Json.h"

#include <optional>
#include <string>

namespace corevideo::modules {

struct CommsRelayResult {
  std::string engineLine;  // set when accepted
  std::string error;       // set when refused (shown verbatim by the app)
};

[[nodiscard]] bool isCommsVerb(const std::string& cmd);

// `lastComms` is the node-shape comms object (commsStateForNode) or nullptr
// when the engine has not reported. `inMeeting` is the node's phase.
[[nodiscard]] CommsRelayResult buildCommsEngineLine(const std::string& cmd, const rpc::Json& command,
                                                    bool inMeeting, const rpc::Json* lastComms);

// The engine's comms_state line, re-keyed for the node: snake_case keys become
// camelCase (meeting_mic -> meetingMic, reach_ok -> reachOk ...) at every
// depth and "cmd" is dropped. Values are untouched. nullopt if not an object.
[[nodiscard]] std::optional<rpc::Json> commsStateForNode(const rpc::Json& engineState);

}  // namespace corevideo::modules
