// ============================================================================
// CoreVideo Pro — SYNTHETIC Zoom capture engine (a fake `corevideo-zoom-engine`)
// ----------------------------------------------------------------------------
// This executable speaks the EXACT same named-pipe + shared-memory IPC protocol
// as the real Zoom engine (native/zoom-engine/engine/main.cpp), but contains NO
// Zoom SDK dependency. It fabricates a multi-participant meeting so we can
// reproduce and validate the multi-participant crash/freeze scenario WITHOUT a
// real Zoom call.
//
// What it does, mirroring the real engine's wire shapes:
//   • Creates the two named pipes (P2E inbound, E2P outbound) and waits for the
//     core to connect — exactly like main.cpp's ipc_setup().
//   • Emits {"cmd":"ready"} on connect.
//   • On `init`  → emits {"cmd":"auth_ok"}.
//   • On `join`  → emits {"cmd":"joined"} then a {"cmd":"participants",...}
//                  roster with 3 participants (numeric ids, has_video=true,
//                  names "mv1","mv2","Producer").
//   • On `subscribe` (mode != screenshare) → starts writing synthetic I420
//     frames to that source's shared-memory region at ~30 fps and emits the
//     matching {"cmd":"frame",...} events with the exact JSON shape the core
//     expects (source_uuid, participant_id, w, h).
//   • Churn (this is what fail-fasts the WinUI):
//       - every ~2s rotates the active_speaker,
//       - every ~10s a 4th participant ("mv4") leaves + re-joins so the roster
//         oscillates 3 <-> 4, driving multiview tile add/remove churn.
//
// Extra robustness for headless validation: by default it AUTO-subscribes every
// video-on participant (source_uuid "participant-video-<id>-auto") so frames
// flow into the core's I420->GPU convert + compositor path even if the parent
// never sends explicit `subscribe` commands. Disable with
// COREVIDEO_FAKE_ENGINE_AUTOSUBSCRIBE=0.
//
// Env knobs:
//   COREVIDEO_FAKE_ENGINE_AUTOSUBSCRIBE  "1" (default) / "0"
//   COREVIDEO_FAKE_ENGINE_RES            auto-target resolution: 0=360p 1=720p 2=1080p (default 2)
//   COREVIDEO_FAKE_ENGINE_PARTICIPANTS   baseline participant count (default 3)
//   COREVIDEO_FAKE_ENGINE_LOG            optional path for a standalone diag log
//
// Windows-focused; a minimal POSIX fallback keeps the file portable so the
// CMake target stays green on the Linux stub build.
// ============================================================================

// engine-ipc.h selects its platform branch on `WIN32` (which CMake defines for
// Windows targets but a bare `cl` invocation does not). Mirror that define from
// the standard `_WIN32` so this file builds identically under CMake or direct cl.
#if defined(_WIN32) && !defined(WIN32)
#define WIN32 1
#endif

#include "engine-ipc.h"
#include "engine-writer.h"  // EngineIpc::init / EngineIpc::write (thread-safe)

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

// ── Standalone diagnostic log (independent of the core's media-core.log) ──────
static void diag(const std::string& line) {
    const char* path = std::getenv("COREVIDEO_FAKE_ENGINE_LOG");
    if (!path || !*path) return;
    std::ofstream out(path, std::ios::app);
    if (out) out << line << "\n";
}

// ── Minimal JSON field extraction (matches main.cpp's helpers) ───────────────
static std::string json_str(const std::string& json, const std::string& key) {
    const std::string needle = "\"" + key + "\":\"";
    auto pos = json.find(needle);
    if (pos == std::string::npos) return {};
    pos += needle.size();
    std::string result;
    while (pos < json.size()) {
        char c = json[pos++];
        if (c == '\\') { if (pos < json.size()) pos++; continue; }
        if (c == '"') break;
        result += c;
    }
    return result;
}

static uint32_t json_uint(const std::string& json, const std::string& key) {
    const std::string needle = "\"" + key + "\":";
    auto pos = json.find(needle);
    if (pos == std::string::npos) return 0;
    pos += needle.size();
    try { return static_cast<uint32_t>(std::stoul(json.substr(pos))); }
    catch (...) { return 0; }
}

static std::string json_escape(const std::string& in) {
    std::string out;
    out.reserve(in.size() + 8);
    for (char c : in) {
        switch (c) {
        case '\\': out += "\\\\"; break;
        case '"': out += "\\\""; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default: out += c; break;
        }
    }
    return out;
}

// ── Platform pipe setup (mirrors main.cpp; no message pump needed — no COM) ───
#if defined(WIN32)
static bool ipc_connect_blocking(HANDLE pipe) {
    BOOL ok = ConnectNamedPipe(pipe, nullptr);
    return ok || GetLastError() == ERROR_PIPE_CONNECTED;
}

static bool ipc_setup(IpcFd& p2e, IpcFd& e2p) {
    // Engine CREATES the pipes (server) and the core CONNECTS (client) — this is
    // the same ownership split as the real engine.
    p2e = CreateNamedPipeA(PIPE_P2E, PIPE_ACCESS_INBOUND,
                           PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
                           1, 65536, 65536, 0, nullptr);
    e2p = CreateNamedPipeA(PIPE_E2P, PIPE_ACCESS_OUTBOUND,
                           PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
                           1, 65536, 65536, 0, nullptr);
    if (p2e == INVALID_HANDLE_VALUE || e2p == INVALID_HANDLE_VALUE) return false;
    // Core's connectIpc() opens P2E (write) then E2P (read); accept both.
    if (!ipc_connect_blocking(p2e) || !ipc_connect_blocking(e2p)) return false;
    return true;
}

static void ipc_teardown(IpcFd p2e, IpcFd e2p) {
    CloseHandle(p2e);
    CloseHandle(e2p);
}
#else  // POSIX fallback (keeps the CMake target portable for the stub build)
#  include <sys/socket.h>
#  include <sys/un.h>
#  include <unistd.h>
#  include <cstring>
static int unix_listen(const char* path) {
    int srv = socket(AF_UNIX, SOCK_STREAM, 0);
    if (srv < 0) return -1;
    struct sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    std::strncpy(addr.sun_path, path, sizeof(addr.sun_path) - 1);
    unlink(path);
    if (bind(srv, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0 ||
        listen(srv, 1) < 0) { close(srv); return -1; }
    return srv;
}
static bool ipc_setup(IpcFd& p2e, IpcFd& e2p) {
    int sp = unix_listen(SOCK_P2E), se = unix_listen(SOCK_E2P);
    if (sp < 0 || se < 0) { if (sp >= 0) close(sp); if (se >= 0) close(se); return false; }
    p2e = accept(sp, nullptr, nullptr);
    e2p = accept(se, nullptr, nullptr);
    close(sp); close(se);
    return p2e >= 0 && e2p >= 0;
}
static void ipc_teardown(IpcFd p2e, IpcFd e2p) {
    close(p2e); close(e2p);
    unlink(SOCK_P2E); unlink(SOCK_E2P);
}
#endif

// ── Resolution → dimensions (0=360p, 1=720p, 2=1080p) ────────────────────────
static void res_to_dims(uint32_t res, uint32_t& w, uint32_t& h) {
    switch (res) {
    case 0: w = 640;  h = 360;  break;
    case 2: w = 1920; h = 1080; break;
    case 1:
    default: w = 1280; h = 720; break;
    }
}

// ── Synthetic meeting state ──────────────────────────────────────────────────
struct Participant {
    uint32_t id = 0;
    std::string name;
    bool has_video = true;
};

struct Target {
    uint32_t participant_id = 0;
    uint32_t resolution = 2;
    std::string source_uuid;
    ShmRegion shm;
    uint64_t frame_count = 0;
    bool is_auto = false;  // created by auto-subscribe vs explicit `subscribe`
};

static std::mutex                 g_mtx;          // guards everything below
static std::vector<Participant>   g_roster;       // current participants
static uint32_t                   g_active_speaker = 0;
static std::map<std::string, Target> g_targets;   // by source_uuid
static bool                       g_joined = false;
static std::atomic<bool>          g_running{true};
static bool                       g_autosubscribe = true;
static uint32_t                   g_auto_res = 2;

// Baseline roster: numeric ids, has_video=true, names per the mission spec.
static std::vector<Participant> baseline_roster(int count) {
    std::vector<Participant> r;
    const char* names[] = {"mv1", "mv2", "Producer", "mv4", "mv5", "mv6"};
    const uint32_t ids[] = {101, 102, 103, 104, 105, 106};
    for (int i = 0; i < count && i < 6; ++i)
        r.push_back({ids[i], names[i], true});
    return r;
}

static bool roster_has(uint32_t id) {
    for (const auto& p : g_roster)
        if (p.id == id) return true;
    return false;
}

// Build + send the {"cmd":"participants",...} event — byte-identical shape to
// the real engine's EngineParticipants::send_roster().
static void send_participants_locked() {
    std::string msg = R"({"cmd":"participants","active_speaker_id":)" +
        std::to_string(g_active_speaker) + R"(,"participants":[)";
    for (size_t i = 0; i < g_roster.size(); ++i) {
        const auto& p = g_roster[i];
        if (i) msg += ",";
        const bool talking = (p.id == g_active_speaker);
        msg += R"({"id":)" + std::to_string(p.id) +
               R"(,"name":")" + json_escape(p.name) +
               R"(","has_video":)" + (p.has_video ? "true" : "false") +
               R"(,"is_talking":)" + (talking ? "true" : "false") +
               R"(,"is_muted":false,"is_sharing_screen":false})";
    }
    msg += "]}";
    EngineIpc::write(msg);
}

// Ensure auto-subscribe targets exist for every video-on participant and prune
// auto targets whose participant has left.
static void sync_auto_targets_locked() {
    if (!g_autosubscribe) return;
    // Add missing.
    for (const auto& p : g_roster) {
        if (!p.has_video) continue;
        const std::string uuid = "participant-video-" + std::to_string(p.id) + "-auto";
        if (g_targets.find(uuid) == g_targets.end()) {
            Target t;
            t.participant_id = p.id;
            t.resolution = g_auto_res;
            t.source_uuid = uuid;
            t.is_auto = true;
            g_targets.emplace(uuid, std::move(t));
            diag("auto-subscribe " + uuid + " res=" + std::to_string(g_auto_res));
        }
    }
    // Prune auto targets for departed participants.
    for (auto it = g_targets.begin(); it != g_targets.end();) {
        if (it->second.is_auto && !roster_has(it->second.participant_id)) {
            shm_region_destroy(it->second.shm);
            diag("auto-unsubscribe " + it->first);
            it = g_targets.erase(it);
        } else {
            ++it;
        }
    }
}

// Fabricate one animated I420 frame for `t` into its SHM, then emit the frame
// event. Replicates engine-video.cpp's odd/even sequence tearing protocol.
static void produce_frame_locked(Target& t, uint64_t tick) {
    uint32_t w = 0, h = 0;
    res_to_dims(t.resolution, w, h);
    const size_t y_len = static_cast<size_t>(w) * static_cast<size_t>(h);
    const size_t total = sizeof(ShmFrameHeader) + y_len + y_len / 4 + y_len / 4;

    if (!t.shm.ptr || t.shm.size < total) {
        const std::string region_name = IPC_SHM_PREFIX + t.source_uuid;
        if (!shm_region_create(t.shm, region_name, total)) {
            if (t.frame_count == 0)
                EngineIpc::write(
                    R"({"cmd":"debug","stage":"video_shm_create_failed","source_uuid":")" +
                    t.source_uuid + R"(","participant_id":)" +
                    std::to_string(t.participant_id) + "}");
            return;
        }
    }

    auto* hdr = static_cast<ShmFrameHeader*>(t.shm.ptr);
    auto* pixels = static_cast<uint8_t*>(t.shm.ptr) + sizeof(ShmFrameHeader);
    uint8_t* yp = pixels;
    uint8_t* up = pixels + y_len;
    uint8_t* vp = pixels + y_len + y_len / 4;

    // Per-participant base luma + chroma so each tile reads as a distinct color,
    // animated by `tick` so motion / fps is visually observable.
    const uint32_t pid = t.participant_id;
    const int base = static_cast<int>(40 + (pid * 37) % 160);
    const int phase = static_cast<int>(tick * 6);
    // Diagonal moving gradient in luma.
    for (uint32_t y = 0; y < h; ++y) {
        uint8_t* row = yp + static_cast<size_t>(y) * w;
        const int yterm = static_cast<int>((y * 255) / h);
        for (uint32_t x = 0; x < w; ++x) {
            int v = base + ((static_cast<int>((x * 255) / w) + yterm + phase) & 0xFF) / 2;
            row[x] = static_cast<uint8_t>(v > 255 ? 255 : (v < 0 ? 0 : v));
        }
    }
    // Chroma: per-participant tint, slowly drifting so it is not perfectly static.
    const uint8_t cu = static_cast<uint8_t>(90 + (pid * 53 + tick) % 110);
    const uint8_t cv = static_cast<uint8_t>(90 + (pid * 97 + tick / 2) % 110);
    std::memset(up, cu, y_len / 4);
    std::memset(vp, cv, y_len / 4);

    // Sequence protocol: odd while writing, even when complete (reader rejects odd).
    uint32_t seq = hdr->sequence + 1;
    if ((seq & 1u) == 0) ++seq;
    hdr->sequence = seq;
    std::atomic_thread_fence(std::memory_order_release);
    hdr->width = w;
    hdr->height = h;
    hdr->y_len = static_cast<uint32_t>(y_len);
    // (planes already written above)
    std::atomic_thread_fence(std::memory_order_release);
    hdr->sequence = seq + 1;

    ++t.frame_count;
    if (t.frame_count == 1 || t.frame_count % 120 == 0) {
        EngineIpc::write(
            R"({"cmd":"debug","stage":"video_frame_received","source_uuid":")" +
            t.source_uuid + R"(","participant_id":)" +
            std::to_string(t.participant_id) + R"(,"count":)" +
            std::to_string(t.frame_count) + R"(,"w":)" + std::to_string(w) +
            R"(,"h":)" + std::to_string(h) + "}");
    }

    EngineIpc::write(
        R"({"cmd":"frame","source_uuid":")" + t.source_uuid +
        R"(","participant_id":)" + std::to_string(t.participant_id) +
        R"(,"w":)" + std::to_string(w) + R"(,"h":)" + std::to_string(h) + "}");
}

// ── Producer thread: ~30 fps frame fabrication for all active targets ─────────
static void producer_loop() {
    using clock = std::chrono::steady_clock;
    auto next = clock::now();
    uint64_t tick = 0;
    while (g_running.load(std::memory_order_acquire)) {
        next += std::chrono::milliseconds(33);  // ~30 fps
        {
            std::lock_guard<std::mutex> lk(g_mtx);
            if (g_joined) {
                ++tick;
                for (auto& [uuid, t] : g_targets) {
                    // Only emit frames for participants currently in the meeting —
                    // when a participant leaves, its feed naturally goes stale.
                    if (roster_has(t.participant_id))
                        produce_frame_locked(t, tick);
                }
            }
        }
        std::this_thread::sleep_until(next);
        // If we fell badly behind (debugger pause etc.), resync the cadence.
        if (clock::now() - next > std::chrono::milliseconds(200)) next = clock::now();
    }
}

// ── Churn thread: active-speaker rotation + roster oscillation ────────────────
static void churn_loop() {
    using clock = std::chrono::steady_clock;
    int half_seconds = 0;
    bool fourth_present = false;  // is "mv4" currently in the meeting?
    while (g_running.load(std::memory_order_acquire)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        if (!g_running.load(std::memory_order_acquire)) break;

        bool joined;
        { std::lock_guard<std::mutex> lk(g_mtx); joined = g_joined; }
        if (!joined) continue;

        ++half_seconds;

        // Every ~2s: rotate the active speaker among the current roster.
        if (half_seconds % 4 == 0) {
            std::string speaker_msg;
            {
                std::lock_guard<std::mutex> lk(g_mtx);
                if (!g_roster.empty()) {
                    // advance to the next participant after the current speaker
                    size_t idx = 0;
                    for (size_t i = 0; i < g_roster.size(); ++i)
                        if (g_roster[i].id == g_active_speaker) { idx = i; break; }
                    idx = (idx + 1) % g_roster.size();
                    g_active_speaker = g_roster[idx].id;
                    speaker_msg = R"({"cmd":"active_speaker","participant_id":)" +
                                  std::to_string(g_active_speaker) + "}";
                }
            }
            if (!speaker_msg.empty()) {
                EngineIpc::write(speaker_msg);
                // Refresh roster so is_talking flags follow the active speaker.
                std::lock_guard<std::mutex> lk(g_mtx);
                send_participants_locked();
            }
            diag("active_speaker -> " + std::to_string(g_active_speaker));
        }

        // Every ~10s: oscillate the roster 3 <-> 4 (mv4 leaves / re-joins). This
        // drives multiview tile add/remove churn — the fail-fast trigger.
        if (half_seconds % 20 == 0) {
            std::lock_guard<std::mutex> lk(g_mtx);
            const Participant mv4{104, "mv4", true};
            if (fourth_present) {
                // Leave: drop mv4 from the roster.
                for (auto it = g_roster.begin(); it != g_roster.end(); ++it)
                    if (it->id == mv4.id) { g_roster.erase(it); break; }
                if (g_active_speaker == mv4.id && !g_roster.empty())
                    g_active_speaker = g_roster.front().id;
                fourth_present = false;
                // NOTE: do NOT emit {"cmd":"left"} here. In the real engine
                // protocol `left` means the LOCAL user left the whole meeting —
                // the core's ZoomEngineState maps it to reset() (meetingState ->
                // idle, roster cleared), which flips the WinUI to "Zoom Offline /
                // Video in room (0)" and tears down the meeting ~20s after join.
                // A REMOTE participant leaving is conveyed purely by the updated
                // {"cmd":"participants"} roster sent below — that drives the
                // multiview tile add/remove churn while staying in-meeting.
                diag("roster -> 3 (mv4 left)");
            } else {
                // Re-join: add mv4 back.
                if (!roster_has(mv4.id)) g_roster.push_back(mv4);
                fourth_present = true;
                diag("roster -> 4 (mv4 joined)");
            }
            sync_auto_targets_locked();
            send_participants_locked();
        }
    }
}

// ── Heartbeat thread: {"cmd":"ping"} every ~2s (mirrors the real engine) ──────
static void heartbeat_loop() {
    while (g_running.load(std::memory_order_acquire)) {
        for (int i = 0; i < 20 && g_running.load(std::memory_order_acquire); ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        if (!g_running.load(std::memory_order_acquire)) break;
        if (!EngineIpc::write(R"({"cmd":"ping"})")) break;
    }
}

int main() {
    // Env configuration.
    if (const char* a = std::getenv("COREVIDEO_FAKE_ENGINE_AUTOSUBSCRIBE"))
        g_autosubscribe = !(a[0] == '0');
    if (const char* r = std::getenv("COREVIDEO_FAKE_ENGINE_RES")) {
        try { g_auto_res = static_cast<uint32_t>(std::stoul(r)); } catch (...) {}
        if (g_auto_res > 2) g_auto_res = 2;
    }
    int baseline = 3;
    if (const char* p = std::getenv("COREVIDEO_FAKE_ENGINE_PARTICIPANTS")) {
        try { baseline = std::stoi(p); } catch (...) {}
        if (baseline < 1) baseline = 1;
        if (baseline > 6) baseline = 6;
    }
    diag("fake-engine start autosubscribe=" + std::to_string(g_autosubscribe) +
         " auto_res=" + std::to_string(g_auto_res) +
         " baseline=" + std::to_string(baseline));

    IpcFd p2e = kIpcInvalidFd, e2p = kIpcInvalidFd;
    if (!ipc_setup(p2e, e2p)) {
        diag("ipc_setup failed");
        return 1;
    }
    EngineIpc::init(e2p);
    EngineIpc::write(R"({"cmd":"ready"})");
    diag("ready");

    std::thread producer(producer_loop);
    std::thread churn(churn_loop);
    std::thread heartbeat(heartbeat_loop);

    std::string line;
    while (g_running.load(std::memory_order_acquire)) {
        if (!ipc_read_line(p2e, line)) break;  // EOF / pipe closed
        if (line.empty()) continue;
        diag("recv: " + line);

        if (line.find(IPC_CMD_QUIT) != std::string::npos) {
            g_running.store(false, std::memory_order_release);

        } else if (line.find(IPC_CMD_INIT) != std::string::npos) {
            EngineIpc::write(R"({"cmd":"debug","stage":"init_received"})");
            EngineIpc::write(R"({"cmd":"auth_ok"})");

        } else if (line.find(IPC_CMD_JOIN) != std::string::npos) {
            std::string meeting_id = json_str(line, "meeting_id");
            EngineIpc::write(
                R"({"cmd":"debug","stage":"join_received","meeting_id":")" +
                json_escape(meeting_id) + "\"}");
            {
                std::lock_guard<std::mutex> lk(g_mtx);
                g_roster = baseline_roster(baseline);
                g_active_speaker = g_roster.empty() ? 0 : g_roster.front().id;
                g_joined = true;
                sync_auto_targets_locked();
            }
            EngineIpc::write(R"({"cmd":"joined"})");
            { std::lock_guard<std::mutex> lk(g_mtx); send_participants_locked(); }

        } else if (line.find(IPC_CMD_LEAVE) != std::string::npos) {
            {
                std::lock_guard<std::mutex> lk(g_mtx);
                g_joined = false;
                g_roster.clear();
                g_active_speaker = 0;
                for (auto& [uuid, t] : g_targets) shm_region_destroy(t.shm);
                g_targets.clear();
            }
            EngineIpc::write(R"({"cmd":"left"})");

        } else if (line.find(IPC_CMD_START_MEDIA) != std::string::npos) {
            EngineIpc::write(R"({"cmd":"debug","stage":"raw_media_ready","reason":"fake"})");

        } else if (line.find(IPC_CMD_STOP_MEDIA) != std::string::npos) {
            EngineIpc::write(R"({"cmd":"debug","stage":"raw_media_stopped","reason":"fake"})");

        } else if (line.find(IPC_CMD_SUBSCRIBE_AUDIO) != std::string::npos) {
            // No synthetic audio — acknowledge so the parent's audio path is exercised.
            std::string uuid = json_str(line, "source_uuid");
            EngineIpc::write(
                R"({"cmd":"debug","stage":"audio_subscribe","source_uuid":")" +
                json_escape(uuid) + "\"}");

        } else if (line.find(IPC_CMD_SUBSCRIBE) != std::string::npos) {
            std::string uuid = json_str(line, "source_uuid");
            uint32_t pid = json_uint(line, "participant_id");
            uint32_t res = json_uint(line, "resolution");
            if (res > 2) res = 1;
            const std::string mode = json_str(line, "mode");
            if (!uuid.empty() && pid != 0) {
                std::lock_guard<std::mutex> lk(g_mtx);
                auto it = g_targets.find(uuid);
                if (it == g_targets.end()) {
                    Target t;
                    t.participant_id = pid;
                    t.resolution = res;
                    t.source_uuid = uuid;
                    t.is_auto = false;
                    g_targets.emplace(uuid, std::move(t));
                } else {
                    // Re-subscribe: honor an upgraded resolution.
                    it->second.participant_id = pid;
                    if (res > it->second.resolution) it->second.resolution = res;
                    it->second.is_auto = false;
                }
            }
            EngineIpc::write(
                R"({"cmd":"debug","stage":"video_source_bound","source_uuid":")" +
                json_escape(uuid) + R"(","participant_id":)" + std::to_string(pid) +
                R"(,"requested":)" + std::to_string(res) +
                R"(,"mode":")" + json_escape(mode) + "\"}");

        } else if (line.find(IPC_CMD_UNSUBSCRIBE) != std::string::npos) {
            std::string uuid = json_str(line, "source_uuid");
            std::lock_guard<std::mutex> lk(g_mtx);
            auto it = g_targets.find(uuid);
            if (it != g_targets.end()) {
                shm_region_destroy(it->second.shm);
                g_targets.erase(it);
            }
        }
    }

    g_running.store(false, std::memory_order_release);
    if (producer.joinable()) producer.join();
    if (churn.joinable()) churn.join();
    if (heartbeat.joinable()) heartbeat.join();

    {
        std::lock_guard<std::mutex> lk(g_mtx);
        for (auto& [uuid, t] : g_targets) shm_region_destroy(t.shm);
        g_targets.clear();
    }
    ipc_teardown(p2e, e2p);
    diag("exit");
    return 0;
}
