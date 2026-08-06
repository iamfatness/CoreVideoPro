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
//   COREVIDEO_FAKE_ENGINE_PARTICIPANTS   baseline participant count (default 3, max 16)
//   COREVIDEO_FAKE_ENGINE_FPS            frame cadence (default 30; use 60 for
//                                        the 1080p60 load the product targets)
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

static bool ipc_setup(IpcFd& p2e, IpcFd& e2p, const std::string& token) {
    // Engine CREATES the pipes (server) and the core CONNECTS (client) — this is
    // the same ownership split as the real engine.
    p2e = CreateNamedPipeA(ipc_pipe_p2e(token).c_str(), PIPE_ACCESS_INBOUND,
                           PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
                           1, 65536, 65536, 0, nullptr);
    e2p = CreateNamedPipeA(ipc_pipe_e2p(token).c_str(), PIPE_ACCESS_OUTBOUND,
                           PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
                           1, 65536, 65536, 0, nullptr);
    if (p2e == INVALID_HANDLE_VALUE || e2p == INVALID_HANDLE_VALUE) return false;
    // Core's connectIpc() opens P2E (write) then E2P (read); accept both.
    if (!ipc_connect_blocking(p2e) || !ipc_connect_blocking(e2p)) return false;
    return true;
}

static void ipc_teardown(IpcFd p2e, IpcFd e2p, const std::string& /*token*/) {
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
static bool ipc_setup(IpcFd& p2e, IpcFd& e2p, const std::string& token) {
    const std::string sock_p2e = ipc_sock_p2e(token), sock_e2p = ipc_sock_e2p(token);
    int sp = unix_listen(sock_p2e.c_str()), se = unix_listen(sock_e2p.c_str());
    if (sp < 0 || se < 0) { if (sp >= 0) close(sp); if (se >= 0) close(se); return false; }
    p2e = accept(sp, nullptr, nullptr);
    e2p = accept(se, nullptr, nullptr);
    close(sp); close(se);
    return p2e >= 0 && e2p >= 0;
}
static void ipc_teardown(IpcFd p2e, IpcFd e2p, const std::string& token) {
    close(p2e); close(e2p);
    unlink(ipc_sock_p2e(token).c_str()); unlink(ipc_sock_e2p(token).c_str());
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
    // Precomputed luma plane. Generating the gradient per pixel per frame costs
    // ~2M integer divides per 1080p frame, which capped the rig at ~23fps/source
    // with 10 sources — the PRODUCER, not the core, was the bottleneck, so a
    // 1080p60 load could not actually be delivered. The real engine memcpys an
    // already-decoded I420 frame into SHM, so a cached plane + memcpy is both
    // faster AND a more faithful model of the real write cost.
    std::vector<uint8_t> luma;
    uint32_t luma_w = 0, luma_h = 0;
};

// Z4a (zoom-audio-spec): deterministic TONE emission over the REAL audio ring
// transport so the full pipeline (ring -> core -> feed -> mix -> monitor) can
// be soak-tested without a meeting or an operator ear. Each participant gets a
// distinct frequency (220Hz + pid%8 * 110) - artifacts show as spectral smear,
// clicks, or autocorr echoes against a mathematically known signal.
struct AudioToneTarget {
    uint32_t participant_id = 0;
    double fixedFreq = 0.0;  // >0 overrides the per-participant frequency
    ShmRegion shm;
    uint64_t samplePos = 0;
    uint64_t packetsWritten = 0;
    std::chrono::steady_clock::time_point startedAt;
};

static std::mutex                 g_mtx;          // guards everything below
static std::vector<Participant>   g_roster;       // current participants
static uint32_t                   g_active_speaker = 0;
static std::map<std::string, AudioToneTarget> g_audioTargets;  // Z4a tone streams
static std::map<std::string, Target> g_targets;   // by source_uuid
static bool                       g_joined = false;
static std::atomic<bool>          g_running{true};
static bool                       g_autosubscribe = true;
static uint32_t                   g_auto_res = 2;

// Baseline roster: numeric ids, has_video=true, names per the mission spec.
// The first six keep their historical names/ids so the existing validators
// (validate-record-audio / validate-iso-record) see an unchanged roster; past
// that the roster is generated so a drill can synthesize a FULL production wall
// (the product targets 10 x 1080p sources, which is where the render tick's
// per-source costs actually show up — six was never enough to reproduce it).
static std::vector<Participant> baseline_roster(int count) {
    static const std::string kNames[] = {"mv1", "mv2", "Producer", "mv4", "mv5", "mv6"};
    std::vector<Participant> r;
    for (int i = 0; i < count; ++i) {
        const uint32_t id = static_cast<uint32_t>(101 + i);
        const std::string name = i < 6 ? kNames[i] : ("mv" + std::to_string(i + 1));
        r.push_back({id, name, true});
    }
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
        const std::string region_name = EngineIpc::shm_prefix() + t.source_uuid;
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
    // Build the diagonal gradient ONCE per target, then memcpy it per frame.
    if (t.luma.size() != y_len || t.luma_w != w || t.luma_h != h) {
        t.luma.resize(y_len);
        for (uint32_t y = 0; y < h; ++y) {
            uint8_t* row = t.luma.data() + static_cast<size_t>(y) * w;
            const int yterm = static_cast<int>((y * 255) / h);
            for (uint32_t x = 0; x < w; ++x) {
                int v = base + ((static_cast<int>((x * 255) / w) + yterm) & 0xFF) / 2;
                row[x] = static_cast<uint8_t>(v > 255 ? 255 : (v < 0 ? 0 : v));
            }
        }
        t.luma_w = w;
        t.luma_h = h;
    }
    // Paint the full planes ONCE; afterwards only advance a moving band.
    //
    // Repainting 3.1MB per frame per source made the PRODUCER the bottleneck
    // (profiled: _platform_memmove dominant, ceiling ~690MB/s => ~22fps/source
    // at 8-10 sources), so the rig could not deliver the 1080p60 wall it was
    // supposed to be testing. That write cost is rig overhead, not product cost:
    // in the real system the Zoom SDK's own decode thread fills this buffer. The
    // CORE still snapshots the full 3.1MB every frame, which is the cost under
    // test — so this changes what the rig can deliver, not what it measures.
    if (t.frame_count == 0) {
        std::memcpy(yp, t.luma.data(), y_len);
        std::memset(up, static_cast<uint8_t>(90 + (pid * 53) % 110), y_len / 4);
        std::memset(vp, static_cast<uint8_t>(90 + (pid * 97) % 110), y_len / 4);
    }
    // Motion stays observable (and every frame's bytes differ) via a moving band.
    const uint32_t band = static_cast<uint32_t>((phase / 6) % (h ? h : 1));
    const uint32_t prev = static_cast<uint32_t>((band + h - 1) % (h ? h : 1));
    std::memcpy(yp + static_cast<size_t>(prev) * w,
                t.luma.data() + static_cast<size_t>(prev) * w, w);
    std::memset(yp + static_cast<size_t>(band) * w, 235, w);

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

    // Video-beacon fix: discovery/liveness beacon only (core polls frames).
    if (t.frame_count == 1 || t.frame_count % 30 == 0) {
        EngineIpc::write(
            R"({"cmd":"frame","source_uuid":")" + t.source_uuid +
            R"(","participant_id":)" + std::to_string(t.participant_id) +
            R"(,"w":)" + std::to_string(w) + R"(,"h":)" + std::to_string(h) + "}");
    }
}

// Z4a: emit all 10ms tone packets due for a target through the SHARED ring
// layout (engine-ipc.h) - the same writer protocol as the real engine.
static void produce_audio_locked(const std::string& uuid, AudioToneTarget& target) {
    if (!target.shm.ptr) {
        const std::string region_name = EngineIpc::shm_prefix() + uuid + "_audio";
        if (!shm_region_create(target.shm, region_name, audio_ring_region_size())) return;
        auto* hdr = static_cast<ShmAudioRingHeader*>(target.shm.ptr);
        hdr->slot_count = kAudioRingSlots;
        hdr->slot_payload = kAudioRingSlotPayload;
        hdr->write_counter = 0;
        std::atomic_thread_fence(std::memory_order_release);
        hdr->magic = kAudioRingMagic;
        target.startedAt = std::chrono::steady_clock::now();
    }
    const auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                               std::chrono::steady_clock::now() - target.startedAt).count();
    auto due = static_cast<uint64_t>(elapsedMs / 10);
    // After a producer stall, SPREAD the backlog (at most 8 packets per pass)
    // instead of bursting or SKIPPING - a skip jumps the tone phase and writes
    // a click INTO the packets (soak run 8: 1400 entrance clicks at packet
    // seams, all from this). The 128-slot ring absorbs 8-packet passes; the
    // synthetic timeline simply lags briefly and catches up phase-perfect.
    if (due > target.packetsWritten + 8) {
        due = target.packetsWritten + 8;
    }
    const double freq = target.fixedFreq > 0.0 ? target.fixedFreq
                                               : 220.0 + static_cast<double>(target.participant_id % 8u) * 110.0;
    while (target.packetsWritten < due) {
        auto* hdr = static_cast<ShmAudioRingHeader*>(target.shm.ptr);
        const uint32_t w = hdr->write_counter;
        auto* slotBase = static_cast<char*>(target.shm.ptr) + sizeof(ShmAudioRingHeader) +
                         static_cast<size_t>(w % kAudioRingSlots) * audio_ring_slot_stride();
        auto* slot = reinterpret_cast<ShmAudioRingSlot*>(slotBase);
        slot->seq = 2u * w + 1u;
        std::atomic_thread_fence(std::memory_order_release);
        slot->sample_rate = 48000;
        slot->channels = 1;
        slot->reserved = 0;
        slot->byte_len = 480 * sizeof(int16_t);
        auto* pcm = reinterpret_cast<int16_t*>(slotBase + sizeof(ShmAudioRingSlot));
        for (int i = 0; i < 480; ++i) {
            const double phase = 2.0 * 3.14159265358979323846 * freq *
                                 static_cast<double>(target.samplePos + static_cast<uint64_t>(i)) / 48000.0;
            pcm[i] = static_cast<int16_t>(std::lround(std::sin(phase) * 0.25 * 32767.0));
        }
        target.samplePos += 480;
        std::atomic_thread_fence(std::memory_order_release);
        slot->seq = 2u * w + 2u;
        std::atomic_thread_fence(std::memory_order_release);
        hdr->write_counter = w + 1u;
        ++target.packetsWritten;
        // Discovery beacon only (Z2b): the core drains on its poll.
        if (target.packetsWritten == 1 || target.packetsWritten % 100 == 0) {
            EngineIpc::write(std::string("{\"cmd\":\"audio\",\"source_uuid\":\"") + uuid +
                             "\",\"participant_id\":" + std::to_string(target.participant_id) +
                             ",\"byte_len\":960}");
        }
    }
}

// ── Producer thread: frame fabrication for all active targets ────────────────
// Cadence is COREVIDEO_FAKE_ENGINE_FPS (default 30, back-compat with the audio
// and ISO validators). Zoom delivers up to 1080p60, and the product's north star
// is 8 Zoom + 2 capture @1080p60 — a 30fps rig only exercises HALF the real
// per-frame ingest/upload rate, so perf drills must be able to ask for 60.
static void producer_loop() {
    using clock = std::chrono::steady_clock;
    int fps = 30;
    if (const char* f = std::getenv("COREVIDEO_FAKE_ENGINE_FPS")) {
        try { fps = std::stoi(f); } catch (...) {}
        if (fps < 1) fps = 1;
        if (fps > 120) fps = 120;
    }
    const auto frameInterval = std::chrono::microseconds(1000000 / fps);
    diag("producer fps=" + std::to_string(fps));
    auto next = clock::now();
    uint64_t tick = 0;
    while (g_running.load(std::memory_order_acquire)) {
        next += frameInterval;
        {
            std::lock_guard<std::mutex> lk(g_mtx);
            if (g_joined) {
                ++tick;
                // COREVIDEO_FAKE_NO_VIDEO=1: audio-only mode for the audio soak
                // (4x video synthesis saturated the box: worker 43.6/50 ticks/s
                // -> feed-cap drops at exactly the deficit rate, soak run 17).
                static const bool s_noVideo = [] {
                    const char* env = std::getenv("COREVIDEO_FAKE_NO_VIDEO");
                    return env != nullptr && env[0] == 0x31;
                }();
                if (!s_noVideo) {
                    for (auto& [uuid, t] : g_targets) {
                        if (roster_has(t.participant_id))
                            produce_frame_locked(t, tick);
                    }
                }
                // Z4a: tone packets for every SUBSCRIBED audio target - the app
                // decides which streams exist (ISO per participant + the mixed
                // stream it subscribes on the active-speaker target). A second
                // hardcoded mix stream here interleaved TWO tones into the
                // core zoom-mix buffer = packet-boundary phase chaos (run 11).
                for (auto& [auuid, atarget] : g_audioTargets) {
                    if (atarget.fixedFreq > 0.0 || roster_has(atarget.participant_id))
                        produce_audio_locked(auuid, atarget);
                }
            }
        }
        std::this_thread::sleep_until(next);
        // Achieved cadence: a rig that silently under-delivers invalidates every
        // perf number taken with it, so report what it ACTUALLY produced.
        static auto s_rateStamp = clock::now();
        static uint64_t s_rateTick = 0;
        if (++s_rateTick >= 120) {
            const double sec = std::chrono::duration<double>(clock::now() - s_rateStamp).count();
            diag("producer achieved " + std::to_string(sec > 0 ? 120.0 / sec : 0.0) +
                 " ticks/s across " + std::to_string(g_targets.size()) + " targets");
            s_rateStamp = clock::now();
            s_rateTick = 0;
        }
        // If we fell badly behind (debugger pause etc.), resync the cadence.
        if (clock::now() - next > std::chrono::milliseconds(200)) next = clock::now();
    }
}

// ── Churn thread: active-speaker rotation + roster oscillation ────────────────
static void churn_loop() {
    // Z4b soak mode: a deterministic AUDIO soak needs stable streams - roster
    // churn (a simulation feature for multiview tile testing) ends ISO tone
    // streams mid-capture. COREVIDEO_FAKE_NO_CHURN=1 freezes the roster AND
    // the active-speaker rotation.
    if (const char* noChurn = std::getenv("COREVIDEO_FAKE_NO_CHURN"); noChurn && noChurn[0] == 0x31) {
        return;
    }
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

int main(int argc, char** argv) {
    // Per-instance IPC token from the parent — must match the pipe/socket/SHM
    // names the core's ZoomEngineProcessClient expects (see engine main.cpp).
    const std::string ipc_token = ipc_token_from_args(argc, argv);
    EngineIpc::set_shm_prefix(ipc_token);
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
        // 16 covers the 10-source production wall plus headroom.
        if (baseline > 16) baseline = 16;
    }
    diag("fake-engine start autosubscribe=" + std::to_string(g_autosubscribe) +
         " auto_res=" + std::to_string(g_auto_res) +
         " baseline=" + std::to_string(baseline));

    IpcFd p2e = kIpcInvalidFd, e2p = kIpcInvalidFd;
    if (!ipc_setup(p2e, e2p, ipc_token)) {
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
                // Mixed-audio parity: the REAL engine delivers meeting-mix audio
                // on its active-speaker target without any fake-visible
                // subscription (soak run 12: the app only subscribes ISO, so a
                // subscription-dependent mix never formed). Create the ONE mix
                // stream at join, keyed by a stable active-speaker uuid.
                {
                    auto& mixed = g_audioTargets["participant-video-" +
                                                 std::to_string(g_active_speaker) + "-active-speaker"];
                    mixed.participant_id = g_active_speaker;
                    mixed.fixedFreq = 330.0;
                }
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
                for (auto& [auuid, atarget] : g_audioTargets) shm_region_destroy(atarget.shm);
                g_audioTargets.clear();
            }
            EngineIpc::write(R"({"cmd":"left"})");

        } else if (line.find(IPC_CMD_START_MEDIA) != std::string::npos) {
            EngineIpc::write(R"({"cmd":"debug","stage":"raw_media_ready","reason":"fake"})");
            EngineIpc::write(R"({"cmd":"raw_media_status","active":true,"reason":"fake"})");

        } else if (line.find(IPC_CMD_STOP_MEDIA) != std::string::npos) {
            EngineIpc::write(R"({"cmd":"debug","stage":"raw_media_stopped","reason":"fake"})");
            EngineIpc::write(R"({"cmd":"raw_media_status","active":false,"reason":"fake"})");

        } else if (line.find(IPC_CMD_SUBSCRIBE_AUDIO) != std::string::npos) {
            // Z4a: start a deterministic tone stream over the real ring transport.
            std::string uuid = json_str(line, "source_uuid");
            {
                const uint32_t pid = json_uint(line, "participant_id");
                if (!uuid.empty() && pid != 0) {
                    std::lock_guard<std::mutex> lk(g_mtx);
                    auto& target = g_audioTargets[uuid];
                    target.participant_id = pid;
                    // The app subscribes MIXED audio on the active-speaker
                    // target: fixed 330Hz so the mix stream is deterministic.
                    if (uuid.size() > 15 && uuid.rfind("-active-speaker") == uuid.size() - 15)
                        target.fixedFreq = 330.0;
                }
            }
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
    ipc_teardown(p2e, e2p, ipc_token);
    diag("exit");
    return 0;
}
