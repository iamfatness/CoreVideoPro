// Phase 4 Task 4 (mac-o4c): the GPU-direct bitstream queue, writer thread, TS
// envelope and backpressure on POSIX. Before this task the POSIX encoder sink
// wrote raw Annex B into FFmpeg's stdin from the ENCODER CALLBACK while the
// FFmpeg args already declared a timestamped TS input, so a POSIX GPU-direct
// stream would have muxed garbage, and a wedged FFmpeg blocked the encoder.
//
// The real RTMP sender is driven end to end: a fake GPU encoder (injected
// through setGpuVideoEncoderFactoryForTest) emits one H.264 access unit per
// submitted frame with fixture-chosen timing, and the "ffmpeg" is the posix
// child probe binary in a mode the test picks (COREVIDEO_TEST_PROBE_MODE).
//
// Fixture invariants (plan-authoring rule 1):
//   - frames are 30 fps and sync() is called at 34 ms steps, so the sender's
//     pacer accepts every frame;
//   - the first emitted chunk is a keyframe at pts=dts=1'000'000 (100 ns) and
//     each next chunk is +333'333;
//   - the wedged-reader fake is `stubborn-stream`: the Task 1 encoder probe
//     runs the same fake binary, and a plain `stubborn` probe both blocked each
//     start for its 5 s budget and wrote the pidfile the stop test then read;
//   - PLAN FIXTURE CORRECTED: the plan expected the first PES PTS to be 9000
//     (1'000'000 x 9 / 1000). EncodedVideoTransportStream documents that one
//     per-process epoch - the first video DTS - is subtracted from every PID,
//     so the first access unit carries PTS 0 and the second 3000 (one 30 fps
//     period at 90 kHz).
#if !defined(_WIN32) && !COREVIDEO_STUB && COREVIDEO_ENABLE_DEV_ADAPTERS && COREVIDEO_WITH_RTMP_OUTPUT

#include "modules/BitstreamQueueTestAccess.h"
#include "modules/GpuVideoEncoder.h"
#include "modules/Interfaces.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <cerrno>
#include <unistd.h>

namespace {
namespace fs = std::filesystem;
using namespace corevideo::modules;
using Clock = std::chrono::steady_clock;

struct EncoderLog {
  std::mutex mutex;
  int starts = 0;
  int64_t emitted = 0;
};

// Emits one access unit per submit, synchronously, on the caller's thread.
class FakeTimedEncoder final : public GpuVideoEncoder {
 public:
  FakeTimedEncoder(std::shared_ptr<EncoderLog> log, size_t payloadBytes, int gop)
      : log_(std::move(log)), payloadBytes_(payloadBytes), gop_(gop) {}
  bool start(const GpuVideoEncoderConfig&, GpuEncodedChunkSink sink) override {
    sink_ = std::move(sink);
    std::lock_guard<std::mutex> lock(log_->mutex);
    ++log_->starts;
    return true;
  }
  bool submit(const GpuVideoEncoderFrame&) override {
    const bool key = forceKey_.exchange(false) || (gop_ > 0 && index_ % gop_ == 0);
    std::vector<uint8_t> au(payloadBytes_, 0x5a);
    au[0] = 0;
    au[1] = 0;
    au[2] = 0;
    au[3] = 1;
    au[4] = key ? 0x65 : 0x41;  // IDR slice / non-IDR slice
    GpuEncodedChunk chunk;
    chunk.data = au.data();
    chunk.size = au.size();
    chunk.keyframe = key;
    chunk.frameNumber = index_;
    chunk.pts100ns = 1'000'000 + index_ * 333'333;
    chunk.dts100ns = chunk.pts100ns;
    chunk.duration100ns = 333'333;
    chunk.timingValid = true;
    ++index_;
    if (sink_) sink_(chunk);
    std::lock_guard<std::mutex> lock(log_->mutex);
    ++log_->emitted;
    return true;
  }
  bool requestKeyframe() override {
    forceKey_.store(true);
    return true;
  }
  void stop() override { sink_ = nullptr; }
  bool healthy() const override { return true; }

 private:
  std::shared_ptr<EncoderLog> log_;
  size_t payloadBytes_;
  int gop_;
  int64_t index_ = 0;
  std::atomic<bool> forceKey_{false};
  GpuEncodedChunkSink sink_;
};

struct Rig {
  fs::path dir;
  RtmpOutputSenderForTest sender;
  std::shared_ptr<EncoderLog> log = std::make_shared<EncoderLog>();
  OutputDestinationSettings settings;
  double elapsedMs = 0;
  int64_t frameNumber = 0;
  std::string priorGuard;
  bool hadGuard = false;
  // Program audio handed to sync(): real audio is what selects the unified TS
  // mux (realAudioEnabledForProcess needs activeAudioPresent_).
  bool withAudio = false;
  std::vector<float> pcm = std::vector<float>(1600 * 2, 0.0f);  // 33 ms at 48 kHz stereo

  Rig(const std::string& tag, const std::string& probeMode, size_t payloadBytes, int gop) {
    dir = fs::temp_directory_path() / ("cvp-bitstream-posix-" + tag + "-" + std::to_string(::getpid()));
    fs::remove_all(dir);
    fs::create_directories(dir);
    std::error_code error;
    fs::copy_file(COREVIDEO_POSIX_CHILD_PROBE_PATH, dir / "ffmpeg", fs::copy_options::overwrite_existing, error);
    std::string mode = probeMode;
    const auto at = mode.find("@DIR@");
    if (at != std::string::npos) mode.replace(at, 5, dir.string());
    if (const char* prior = std::getenv("COREVIDEO_CHILD_GUARD_PATH")) priorGuard = prior, hadGuard = true;
    ::setenv("COREVIDEO_TEST_PROBE_MODE", mode.c_str(), 1);
    ::setenv("COREVIDEO_CHILD_GUARD_PATH", COREVIDEO_CHILD_GUARD_PATH, 1);
    sender = createRtmpOutputSenderForTest();
    auto log = this->log;
    sender.queue->setGpuVideoEncoderFactoryForTest([log, payloadBytes, gop] {
      return std::make_unique<FakeTimedEncoder>(log, payloadBytes, gop);
    });
    settings.id = "rtmp";
    settings.label = "RTMP";
    settings.protocol = "rtmp";
    settings.url = "rtmp://127.0.0.1:1935/live";
    settings.streamKey = "posix-test";
    settings.ffmpegBinDirectory = dir.string();
    settings.videoCodec = "h264";
    settings.fps = 30;
  }
  ~Rig() {
    if (sender.sender) (void)sender.sender->sync({}, nullptr, elapsedMs + 1000, {});
    sender.sender.reset();
    ::unsetenv("COREVIDEO_TEST_PROBE_MODE");
    if (hadGuard) ::setenv("COREVIDEO_CHILD_GUARD_PATH", priorGuard.c_str(), 1);
    else ::unsetenv("COREVIDEO_CHILD_GUARD_PATH");
    std::error_code ignored;
    fs::remove_all(dir, ignored);
  }
  OutputSenderSession tick() {
    ProgramFrame frame;
    frame.frameNumber = ++frameNumber;
    frame.width = 320;
    frame.height = 180;
    frame.renderPlanId = "bitstream-posix";
    frame.programFullBgra.width = 320;
    frame.programFullBgra.height = 180;
    frame.programFullBgra.bgra.assign(320u * 180u * 4u, 0x40);
    frame.encoderSharedTexture.iosurfaceId = 77;
    frame.encoderSharedTexture.width = 320;
    frame.encoderSharedTexture.height = 180;
    frame.encoderSharedTexture.frameNumber = frame.frameNumber;
    const auto session = withAudio
        ? sender.sender->sync({"rtmp"}, &frame, elapsedMs, {settings}, &pcm, 2, 48000)
        : sender.sender->sync({"rtmp"}, &frame, elapsedMs, {settings});
    elapsedMs += 34.0;
    return session;
  }
};

std::vector<uint8_t> slurp(const fs::path& path) {
  std::ifstream in(path, std::ios::binary);
  return std::vector<uint8_t>(std::istreambuf_iterator<char>(in), {});
}

// The PTS of every video PES (PID 0x100) start in a TS byte stream, in order.
std::vector<int64_t> videoPesPts(const std::vector<uint8_t>& ts) {
  std::vector<int64_t> pts;
  for (size_t off = 0; off + 188 <= ts.size(); off += 188) {
    const uint8_t* p = ts.data() + off;
    if (p[0] != 0x47) break;
    const bool start = (p[1] & 0x40) != 0;
    const int pid = ((p[1] & 0x1f) << 8) | p[2];
    if (!start || pid != 0x100) continue;
    size_t i = 4;
    if (p[3] & 0x20) i += 1 + p[4];  // adaptation field
    if (i + 14 > 188 || p[i] != 0 || p[i + 1] != 0 || p[i + 2] != 1) continue;
    const uint8_t* t = p + i + 9;
    const int64_t value = (static_cast<int64_t>((t[0] >> 1) & 7) << 30) | (static_cast<int64_t>(t[1]) << 22) |
                          (static_cast<int64_t>(t[2] >> 1) << 15) | (static_cast<int64_t>(t[3]) << 7) |
                          (t[4] >> 1);
    pts.push_back(value);
  }
  return pts;
}

bool alive(pid_t pid) { return pid > 0 && (::kill(pid, 0) == 0 || errno == EPERM); }

TEST(BitstreamQueuePosix, WritesTheTimestampedTsEnvelope) {
  Rig rig("ts", "copy-stdin-to @DIR@/stdin.ts", 256, 0);
  for (int i = 0; i < 8; ++i) (void)rig.tick();
  const fs::path out = rig.dir / "stdin.ts";
  const auto deadline = Clock::now() + std::chrono::seconds(3);
  std::vector<uint8_t> ts;
  while (Clock::now() < deadline) {
    ts = slurp(out);
    if (videoPesPts(ts).size() >= 2) break;
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  ASSERT_GE(ts.size(), 376u) << "the writer delivered no transport stream";
  EXPECT_EQ(ts[0], 0x47) << "TS sync byte at offset 0";
  EXPECT_EQ(ts[188], 0x47) << "TS sync byte at offset 188";
  const auto pts = videoPesPts(ts);
  ASSERT_GE(pts.size(), 2u);
  EXPECT_EQ(pts[0], 0) << "the first access unit carries the epoch (PTS 0)";
  // 333'333 x 100 ns = 2999.997 ticks at 90 kHz: the packetizer ROUNDS to the
  // nearest tick (EncodedVideoTransportStream::ticks), so this also pins that.
  EXPECT_EQ(pts[1], 3000) << "the next one is one 30 fps period later at 90 kHz";
  std::lock_guard<std::mutex> lock(rig.log->mutex);
  EXPECT_EQ(rig.log->starts, 1);
}

TEST(BitstreamQueuePosix, WedgedReaderShedsGopTailsWithoutRebuildingTheEncoder) {
  // stubborn: never reads stdin, ignores SIGTERM. 4 KiB chunks fill the 64 KiB pipe at once.
  Rig rig("wedged", "stubborn-stream @DIR@/stubborn.pid", 4096, 60);
  OutputSenderSession last;
  for (int i = 0; i < 300; ++i) last = rig.tick();
  ASSERT_FALSE(last.senders.empty());
  const auto& row = last.senders.front();
  ASSERT_TRUE(row.backpressure.has_value()) << "a GPU-direct sender publishes its backpressure";
  EXPECT_GT(row.backpressure->discardedChunks, 0) << "the wedged pipe must shed GOP tails";
  EXPECT_NE(row.status, "failed") << "shedding, not failing the sender to its supervisor";
  std::lock_guard<std::mutex> lock(rig.log->mutex);
  EXPECT_EQ(rig.log->starts, 1) << "the encoder is never rebuilt over a congested pipe";
  EXPECT_GE(rig.log->emitted, 290) << "fixture invariant: the encoder really was fed";
}

TEST(BitstreamQueuePosix, StopReturnsWithinTwoSecondsOnAWedgedPipe) {
  Rig rig("stop", "stubborn-stream @DIR@/stubborn.pid", 4096, 60);
  OutputSenderSession last;
  for (int i = 0; i < 60; ++i) last = rig.tick();
  ASSERT_FALSE(last.senders.empty());
  const fs::path pidfile = rig.dir / "stubborn.pid";
  // The stand-in writes its pid just after it is spawned: bounded poll.
  pid_t pid = 0;
  for (const auto deadline = Clock::now() + std::chrono::seconds(3); Clock::now() < deadline;) {
    std::ifstream in(pidfile);
    if (in >> pid && pid > 0 && alive(pid)) break;
    pid = 0;
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  ASSERT_GT(pid, 0) << "the stand-in ffmpeg never started";
  ASSERT_TRUE(alive(pid)) << "pid " << pid << " already gone before stop";
  const auto started = Clock::now();
  (void)rig.sender.sender->sync({}, nullptr, rig.elapsedMs, {});
  const auto took = Clock::now() - started;
  EXPECT_LT(took, std::chrono::milliseconds(2000))
      << "stop took " << std::chrono::duration_cast<std::chrono::milliseconds>(took).count()
      << " ms with the writer parked on a full pipe";
  const auto reapDeadline = Clock::now() + std::chrono::milliseconds(1000);
  while (alive(pid) && Clock::now() < reapDeadline) std::this_thread::sleep_for(std::chrono::milliseconds(20));
  EXPECT_FALSE(alive(pid)) << "the stand-in ffmpeg outlived stop";
  if (alive(pid)) ::kill(pid, SIGKILL);
}

// Review: the POSIX failure path. An FFmpeg that exits at once breaks the
// pipe (EPIPE; SIGPIPE is ignored process-wide). The writer records a
// PipeWrite failure, the next frame fails the sender to its supervisor, and
// Stop returns at once - nothing is left parked on a dead pipe.
TEST(BitstreamQueuePosix, AnFfmpegThatExitsBreaksThePipeAndFailsTheSender) {
  Rig rig("broken", "exit 3", 4096, 60);
  std::string status;
  for (int i = 0; i < 300 && status != "failed"; ++i) {
    const auto session = rig.tick();
    if (!session.senders.empty()) status = session.senders.front().status;
    std::this_thread::sleep_for(std::chrono::milliseconds(2));  // let the writer meet the dead pipe
  }
  EXPECT_EQ(status, "failed") << "a broken pipe must fail the sender, not leave it streaming into nothing";
  const auto started = Clock::now();
  (void)rig.sender.sender->sync({}, nullptr, rig.elapsedMs, {});
  EXPECT_LT(Clock::now() - started, std::chrono::milliseconds(1500)) << "stop after a broken pipe is prompt";
}

// Phase 4 O5: with GPU-direct and the shared AAC on, POSIX takes the Windows
// one-clock mux (#538 Slice 8): audio rides the TS on stdin, so FFmpeg gets no
// second (pipe:3) input. The fake ffmpeg writes its argv and exits.
TEST(OutputSenderAdapter, PosixGpuDirectUsesUnifiedTs) {
  Rig rig("unified", "argv-to @DIR@/argv.txt", 256, 0);
  rig.sender.sender->setSharedAacEnabled(true);
  rig.withAudio = true;
  OutputSenderSession last;
  for (int i = 0; i < 4; ++i) last = rig.tick();
  ASSERT_FALSE(last.senders.empty());
  const auto& row = last.senders.front();
  ASSERT_TRUE(row.streamClock.has_value()) << "the stream clock is published on POSIX now";
  EXPECT_EQ(row.streamClock->muxInput, "unified-ts") << "status=" << row.status << " code=" << row.lastResultCode;
  std::string argv;
  for (const auto deadline = Clock::now() + std::chrono::seconds(3); Clock::now() < deadline;) {
    std::ifstream in(rig.dir / "argv.txt");
    std::stringstream buffer;
    buffer << in.rdbuf();
    argv = buffer.str();
    if (argv.find("pipe:0") != std::string::npos) break;  // the stream ffmpeg, not the encoder probe
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  ASSERT_NE(argv.find("pipe:0"), std::string::npos) << "fixture invariant: the stream ffmpeg's argv was captured";
  // ONE input. Checking only for "pipe:3" missed the real defect: on POSIX the
  // stray second input was `-f aac -i pipe:0`, and FFmpeg then split stdin
  // between the TS and an AAC demuxer (found by the Task 6 end-to-end gate).
  size_t inputs = 0;
  for (size_t at = argv.find(" -i "); at != std::string::npos; at = argv.find(" -i ", at + 1)) ++inputs;
  EXPECT_EQ(inputs, 1u) << "exactly one FFmpeg input (the TS): " << argv;
  EXPECT_EQ(argv.find("-f aac"), std::string::npos) << "no separate AAC demuxer: " << argv;
  EXPECT_NE(argv.find("-map 0:a:0"), std::string::npos) << "audio is mapped from the TS itself: " << argv;
  EXPECT_EQ(argv.find("pipe:3"), std::string::npos) << "no second audio pipe: " << argv;
}

}  // namespace

#endif
