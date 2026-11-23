// Which handler a P2E line goes to in the macOS engine's reader loop.
//
// Comms lines route FIRST, before engine_parse_command, matching the order in
// the Windows engine (main.cpp hands every comms_* line to EngineComms before
// its own dispatch). Mac dispatch is by exact command name, so a comms line
// that merely mentions an engine verb in a value cannot misfire there anyway;
// the order is kept for parity (EngineLineRouteTest pins both facts).
#pragma once

#include <string>

#include "comms/comms_protocol.h"
#include "engine-command.h"

enum class EngineLineRoute { Comms, Engine };

inline EngineLineRoute routeEngineLine(const std::string& line) {
  return edgecomms::IsCommsCommand(line) ? EngineLineRoute::Comms : EngineLineRoute::Engine;
}
