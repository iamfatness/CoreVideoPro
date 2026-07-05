#pragma once
#include <string>
#include <cstddef>
#include <cstdint>
#include <cerrno>

// ── IPC command / event tokens ────────────────────────────────────────────────
#define IPC_CMD_INIT        "init"
#define IPC_CMD_JOIN        "join"
#define IPC_CMD_LEAVE       "leave"
#define IPC_CMD_SUBSCRIBE   "subscribe"
#define IPC_CMD_SUBSCRIBE_AUDIO "subscribe_audio"
#define IPC_CMD_UNSUBSCRIBE "unsubscribe"
#define IPC_CMD_START_MEDIA "start_media"
#define IPC_CMD_STOP_MEDIA  "stop_media"
#define IPC_CMD_QUIT        "quit"
#define IPC_EVT_READY       "ready"
#define IPC_EVT_AUTH_OK     "auth_ok"
#define IPC_EVT_AUTH_FAIL   "auth_fail"
#define IPC_EVT_JOINED      "joined"
#define IPC_EVT_LEFT        "left"
#define IPC_EVT_FRAME       "frame"
#define IPC_EVT_AUDIO       "audio"
#define IPC_EVT_ERROR       "error"

// Shared-memory name prefix (no leading slash — added per-platform below)
#define IPC_SHM_PREFIX "ZoomObsPlugin_"

struct ShmFrameHeader {
    uint32_t sequence;
    uint32_t width;
    uint32_t height;
    uint32_t y_len;
};
struct ShmAudioHeader {
    uint32_t sequence;
    uint32_t sample_rate;
    uint16_t channels;
    uint16_t reserved;
    uint32_t byte_len;
};

// Audio RING (2026-07-05, owner-heard garbled Zoom audio): the single-slot
// ShmAudioHeader snapshot was overwritten every 10ms packet - whenever the
// consumer pipe-event handling lagged even one packet, audio was lost,
// duplicated, or torn. Video can latest-wins; AUDIO CANNOT. 32 sequenced
// slots give the reader ~320ms of lossless catch-up.
struct ShmAudioRingHeader {
    uint32_t magic;                   // CVAR = 0x43564152; guards layout mismatch
    volatile uint32_t write_counter;  // packets written; slot index = counter % slot_count
    uint32_t slot_count;
    uint32_t slot_payload;            // payload bytes per slot
};
struct ShmAudioRingSlot {
    volatile uint32_t seq;  // 2*counter+1 while writing, 2*counter+2 complete
    uint32_t byte_len;
    uint32_t sample_rate;
    uint16_t channels;
    uint16_t reserved;
    // int16 interleaved PCM payload follows (slot_payload bytes reserved)
};
enum : uint32_t { kAudioRingMagic = 0x43564152u, kAudioRingSlots = 128u, kAudioRingSlotPayload = 4096u };  // 1.28s catch-up: soak-measured pipe-stall bursts lapped 32 slots
inline size_t audio_ring_slot_stride() { return sizeof(ShmAudioRingSlot) + kAudioRingSlotPayload; }
inline size_t audio_ring_region_size() {
    return sizeof(ShmAudioRingHeader) + kAudioRingSlots * audio_ring_slot_stride();
}

// ── Instance-scoped IPC naming ────────────────────────────────────────────────
// The named pipes / unix sockets / shared-memory regions all share the
// "ZoomObsPlugin_" base name this engine inherited from the OBS Zoom plugin it
// grew out of. A per-instance token is spliced into every name so a CoreVideo
// Pro instance never rendezvous with — or gets starved by — another process
// holding the bare base name. Most importantly that other process is the OBS
// zoom plugin: without a token our parent's WaitNamedPipe/CreateFile races OBS's
// singleton pipe server and every join fails with "Timed out connecting to Zoom
// engine IPC" whenever OBS is running. An empty token reproduces the legacy
// fixed names (kept so any un-tokenised caller still works).
#if defined(WIN32)
#  include <windows.h>
#endif

inline std::string ipc_instance_infix(const std::string &token)
{
    return token.empty() ? std::string() : token + "_";
}

#if defined(WIN32)
inline std::string ipc_pipe_p2e(const std::string &token)
{
    return "\\\\.\\pipe\\ZoomObsPlugin_" + ipc_instance_infix(token) + "P2E";
}
inline std::string ipc_pipe_e2p(const std::string &token)
{
    return "\\\\.\\pipe\\ZoomObsPlugin_" + ipc_instance_infix(token) + "E2P";
}
#else
inline std::string ipc_sock_p2e(const std::string &token)
{
    return "/tmp/ZoomObsPlugin_" + ipc_instance_infix(token) + "P2E.sock";
}
inline std::string ipc_sock_e2p(const std::string &token)
{
    return "/tmp/ZoomObsPlugin_" + ipc_instance_infix(token) + "E2P.sock";
}
#endif

inline std::string ipc_shm_prefix(const std::string &token)
{
    return std::string(IPC_SHM_PREFIX) + ipc_instance_infix(token);
}

// Parse "--ipc-token <value>" or "--ipc-token=<value>" out of argv; returns the
// empty string when absent. Shared by the real and fake engine entry points so
// both agree with the parent's ZoomEngineProcessClient on the instance token.
inline std::string ipc_token_from_args(int argc, char **argv)
{
    const std::string flag = "--ipc-token";
    for (int i = 1; i < argc && argv[i]; ++i) {
        const std::string arg = argv[i];
        if (arg == flag && i + 1 < argc && argv[i + 1]) return argv[i + 1];
        if (arg.rfind(flag + "=", 0) == 0) return arg.substr(flag.size() + 1);
    }
    return {};
}

// ── Platform-agnostic file-descriptor type ───────────────────────────────────
#if defined(WIN32)
   using IpcFd = HANDLE;
   // INVALID_HANDLE_VALUE is a reinterpret_cast and not a constant expression
   // on MSVC — use inline const instead of constexpr.
   inline const IpcFd kIpcInvalidFd = INVALID_HANDLE_VALUE;
#else
#  include <unistd.h>
   using IpcFd = int;
   static constexpr IpcFd kIpcInvalidFd = -1;
#endif

// ── Shared-memory region ──────────────────────────────────────────────────────
#if defined(WIN32)
   struct ShmRegion {
       HANDLE  map_handle = nullptr;
       void   *ptr        = nullptr;
       size_t  size       = 0;
   };

   inline void shm_region_destroy(ShmRegion &r)
   {
       if (r.ptr)        { UnmapViewOfFile(r.ptr);   r.ptr        = nullptr; }
       if (r.map_handle) { CloseHandle(r.map_handle); r.map_handle = nullptr; }
       r.size = 0;
   }

   inline bool shm_region_create(ShmRegion &r, const std::string &name, size_t size)
   {
       shm_region_destroy(r);
       r.map_handle = CreateFileMappingA(INVALID_HANDLE_VALUE, nullptr,
                                         PAGE_READWRITE, 0,
                                         static_cast<DWORD>(size), name.c_str());
       if (!r.map_handle) return false;
       r.ptr  = MapViewOfFile(r.map_handle, FILE_MAP_WRITE, 0, 0, size);
       r.size = r.ptr ? size : 0;
       if (!r.ptr) { shm_region_destroy(r); return false; }
       return true;
   }

   inline bool shm_region_open_read(ShmRegion &r, const std::string &name, size_t size)
   {
       shm_region_destroy(r);
       r.map_handle = OpenFileMappingA(FILE_MAP_READ, FALSE, name.c_str());
       if (!r.map_handle) return false;
       r.ptr = MapViewOfFile(r.map_handle, FILE_MAP_READ, 0, 0, size);
       r.size = r.ptr ? size : 0;
       if (!r.ptr) { shm_region_destroy(r); return false; }
       return true;
   }
#else
#  include <sys/mman.h>
#  include <fcntl.h>
   struct ShmRegion {
       int         fd   = -1;
       void       *ptr  = nullptr;
       size_t      size = 0;
       std::string name; // stored so we can shm_unlink on destroy
       bool        owner = false;
   };

   inline void shm_region_destroy(ShmRegion &r)
   {
       if (r.ptr && r.ptr != MAP_FAILED) { munmap(r.ptr, r.size); r.ptr = nullptr; }
       if (r.fd >= 0) {
           close(r.fd);
           if (r.owner && !r.name.empty()) shm_unlink(r.name.c_str());
           r.fd = -1;
       }
       r.size = 0;
       r.name.clear();
       r.owner = false;
   }

   inline bool shm_region_create(ShmRegion &r, const std::string &name, size_t size)
   {
       shm_region_destroy(r);
       r.name = "/" + name; // shm_open requires a leading '/'
       r.owner = true;
       r.fd   = shm_open(r.name.c_str(), O_CREAT | O_RDWR, 0600);
       if (r.fd < 0) return false;
       if (ftruncate(r.fd, static_cast<off_t>(size)) < 0) { shm_region_destroy(r); return false; }
       r.ptr  = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, r.fd, 0);
       r.size = (r.ptr != MAP_FAILED) ? size : 0;
       if (r.ptr == MAP_FAILED) { r.ptr = nullptr; shm_region_destroy(r); return false; }
       return true;
   }

   inline bool shm_region_open_read(ShmRegion &r, const std::string &name, size_t size)
   {
       shm_region_destroy(r);
       r.name = "/" + name;
       r.owner = false;
       r.fd = shm_open(r.name.c_str(), O_RDONLY, 0600);
       if (r.fd < 0) return false;
       r.ptr = mmap(nullptr, size, PROT_READ, MAP_SHARED, r.fd, 0);
       r.size = (r.ptr != MAP_FAILED) ? size : 0;
       if (r.ptr == MAP_FAILED) { r.ptr = nullptr; shm_region_destroy(r); return false; }
       return true;
   }
#endif

// ── Line-oriented I/O helpers ─────────────────────────────────────────────────
// Returns false on EOF, I/O error, or if the line exceeds max_len bytes.
static inline bool ipc_read_line(IpcFd fd, std::string &out,
                                 size_t max_len = 65536)
{
    out.clear();
#if defined(WIN32)
    char ch; DWORD n;
    while (ReadFile(fd, &ch, 1, &n, nullptr) && n == 1) {
        if (ch == '\n') return true;
        if (out.size() >= max_len) return false; // line too long
        out += ch;
    }
    return false; // EOF or error
#else
    char ch; ssize_t n;
    while ((n = read(fd, &ch, 1)) == 1) {
        if (ch == '\n') return true;
        if (out.size() >= max_len) return false; // line too long
        out += ch;
    }
    return false; // EOF (n==0) or error (n==-1)
#endif
}

// Writes msg followed by '\n', retrying short writes until the whole line is
// delivered. Returns true only if every byte was written; false on a closed,
// full, or broken pipe (or an invalid fd). Callers must treat false as a lost
// message and tear down / recover the link rather than assuming delivery.
static inline bool ipc_write_line(IpcFd fd, const std::string &msg)
{
    if (fd == kIpcInvalidFd) return false;
    const std::string out = msg + "\n";
    const char *p   = out.c_str();
    size_t      rem = out.size();
#if defined(WIN32)
    while (rem > 0) {
        DWORD written = 0;
        if (!WriteFile(fd, p, static_cast<DWORD>(rem), &written, nullptr))
            return false;            // pipe closed / broken
        if (written == 0) return false; // no progress — treat as failure
        p   += written;
        rem -= written;
    }
    return true;
#else
    while (rem > 0) {
        ssize_t n = write(fd, p, rem);
        if (n < 0) {
            if (errno == EINTR) continue; // interrupted — retry
            return false;                 // EPIPE / EBADF / etc.
        }
        if (n == 0) return false;         // no progress — treat as failure
        p   += static_cast<size_t>(n);
        rem -= static_cast<size_t>(n);
    }
    return true;
#endif
}
