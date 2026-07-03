#pragma once
// Thread-safe IPC write for the engine process.
// The Zoom SDK fires auth, meeting, participant, video, and audio callbacks on
// its own internal threads concurrently with the main IPC read loop.  All of
// them write to the same e2p fd — serialise those writes here.
#include "engine-ipc.h"
#include <mutex>
#include <string>

namespace EngineIpc {

inline IpcFd &fd()
{
    static IpcFd instance = kIpcInvalidFd;
    return instance;
}

inline std::mutex &mtx()
{
    static std::mutex instance;
    return instance;
}

// Call once from main() after e2p is established, before SDK callbacks start.
inline void init(IpcFd e2p) { fd() = e2p; }

// Per-instance shared-memory name prefix. Defaults to the legacy fixed prefix;
// set_shm_prefix() is called once from main() from the --ipc-token so every SHM
// region this engine creates is isolated to its app instance (mirrors the
// per-instance pipe names). Must be set before any SDK callback creates a
// region — same ordering contract as init().
inline std::string &shm_prefix()
{
    static std::string instance = IPC_SHM_PREFIX;
    return instance;
}
inline void set_shm_prefix(const std::string &token) { shm_prefix() = ipc_shm_prefix(token); }

// Serialised write — safe to call from any thread. Returns false if the line
// could not be fully delivered (closed/broken/full pipe), so callers in the
// engine can stop emitting into a dead link.
inline bool write(const std::string &msg)
{
    std::lock_guard<std::mutex> lk(mtx());
    return ipc_write_line(fd(), msg);
}

} // namespace EngineIpc
