#pragma once
#include "engine-ipc.h"
#include <algorithm>
#include <set>
#include <string>

// Pure, platform-neutral command classification for the engine's stdin loop.
// Dispatch is by the parsed "cmd" value (ipc_command_is), never by substring:
// a subscribe line whose payload contains "join" is a subscribe.
enum class EngineCommand {
    Unknown, Init, Join, Leave, StartMedia, StopMedia,
    SubscribeAudio, Unsubscribe, Subscribe, Quit,
};

inline EngineCommand engine_parse_command(const std::string &line)
{
    struct Entry { const char *name; EngineCommand cmd; };
    static const Entry table[] = {
        {IPC_CMD_INIT, EngineCommand::Init},
        {IPC_CMD_JOIN, EngineCommand::Join},
        {IPC_CMD_LEAVE, EngineCommand::Leave},
        {IPC_CMD_START_MEDIA, EngineCommand::StartMedia},
        {IPC_CMD_STOP_MEDIA, EngineCommand::StopMedia},
        {IPC_CMD_SUBSCRIBE_AUDIO, EngineCommand::SubscribeAudio},
        {IPC_CMD_UNSUBSCRIBE, EngineCommand::Unsubscribe},
        {IPC_CMD_SUBSCRIBE, EngineCommand::Subscribe},
        {IPC_CMD_QUIT, EngineCommand::Quit},
    };
    for (const auto &e : table)
        if (ipc_command_is(line, e.name)) return e.cmd;
    return EngineCommand::Unknown;
}

// The raw "cmd" string value (for logging unknown commands); empty if absent
// or unterminated. Capped so a hostile line cannot bloat the log.
inline std::string engine_command_name(const std::string &line)
{
    const auto key = line.find("\"cmd\"");
    if (key == std::string::npos) return {};
    auto v = line.find(':', key + 5);
    if (v == std::string::npos) return {};
    ++v;
    while (v < line.size() && (line[v] == ' ' || line[v] == '\t' ||
                               line[v] == '\r' || line[v] == '\n')) ++v;
    if (v >= line.size() || line[v] != '"') return {};
    ++v;
    const auto end = line.find('"', v);
    if (end == std::string::npos) return {};
    return line.substr(v, std::min<size_t>(end - v, 64));
}
