// Task 1 (mac-o3): the POSIX ffmpeg encoder probe. Before this task it
// returned true for every encoder, so these tests drive the real probe against
// a stand-in "ffmpeg" (the posix-child probe binary, mode chosen by
// COREVIDEO_TEST_PROBE_MODE) whose exit status and lifetime the fixture
// controls, plus an opt-in run against a real bundled ffmpeg.
#if !defined(_WIN32)

#include "modules/PosixFfmpegEncoderProbe.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <future>
#include <sstream>
#include <string>
#include <thread>

#include <cerrno>
#include <unistd.h>

namespace {
namespace fs = std::filesystem;
using corevideo::modules::posixFfmpegEncoderIsAvailable;
using Clock = std::chrono::steady_clock;

// A temp dir holding a copy of the probe binary named "ffmpeg", with the guard
// pinned so the probe always runs guarded (the product path).
struct FakeFfmpeg {
  fs::path dir;
  fs::path ffmpeg;
  std::string priorGuard;
  bool hadPriorGuard = false;
  FakeFfmpeg() {
    if (const char* prior = std::getenv("COREVIDEO_CHILD_GUARD_PATH")) {
      hadPriorGuard = true;
      priorGuard = prior;
    }
    dir = fs::temp_directory_path() / ("cvp-encprobe-" + std::to_string(::getpid()) + "-" +
                                       std::to_string(Clock::now().time_since_epoch().count()));
    fs::create_directories(dir);
    ffmpeg = dir / "ffmpeg";
    std::error_code error;
    fs::copy_file(COREVIDEO_POSIX_CHILD_PROBE_PATH, ffmpeg, fs::copy_options::overwrite_existing, error);
    ::setenv("COREVIDEO_CHILD_GUARD_PATH", COREVIDEO_CHILD_GUARD_PATH, 1);
  }
  ~FakeFfmpeg() {
    ::unsetenv("COREVIDEO_TEST_PROBE_MODE");
    if (hadPriorGuard) {
      ::setenv("COREVIDEO_CHILD_GUARD_PATH", priorGuard.c_str(), 1);
    } else {
      ::unsetenv("COREVIDEO_CHILD_GUARD_PATH");
    }
    std::error_code error;
    fs::remove_all(dir, error);
  }
  void mode(const std::string& spec) const { ::setenv("COREVIDEO_TEST_PROBE_MODE", spec.c_str(), 1); }
};

std::string slurp(const fs::path& path) {
  std::ifstream in(path);
  std::stringstream buffer;
  buffer << in.rdbuf();
  return buffer.str();
}

bool alive(pid_t pid) { return pid > 0 && (::kill(pid, 0) == 0 || errno == EPERM); }

TEST(PosixEncoderProbe, TrueOnlyOnExitZero) {
  FakeFfmpeg fake;
  ASSERT_TRUE(fs::exists(fake.ffmpeg));
  fake.mode("exit 0");
  EXPECT_TRUE(posixFfmpegEncoderIsAvailable(fake.ffmpeg.string(), "h264_videotoolbox"));
  fake.mode("exit 1");
  EXPECT_FALSE(posixFfmpegEncoderIsAvailable(fake.ffmpeg.string(), "h264_videotoolbox"));
  fake.mode("exit 187");  // what ffmpeg returns when VideoToolbox cannot open a session
  EXPECT_FALSE(posixFfmpegEncoderIsAvailable(fake.ffmpeg.string(), "hevc_videotoolbox"));
}

TEST(PosixEncoderProbe, AMissingExecutableIsNotAvailable) {
  EXPECT_FALSE(posixFfmpegEncoderIsAvailable("/nonexistent/cvp/ffmpeg", "h264_videotoolbox"));
  EXPECT_FALSE(posixFfmpegEncoderIsAvailable("", "h264_videotoolbox"));
}

TEST(PosixEncoderProbe, TimesOutAStubbornChildAndReapsIt) {
  FakeFfmpeg fake;
  const fs::path pidfile = fake.dir / "stubborn.pid";
  fake.mode("stubborn " + pidfile.string());  // ignores SIGTERM, sleeps 120 s

  const auto started = Clock::now();
  auto call = std::async(std::launch::async, [&] {
    return posixFfmpegEncoderIsAvailable(fake.ffmpeg.string(), "hevc_videotoolbox");
  });

  // Fixture invariant: the stubborn child must exist and be alive BEFORE the
  // 5 s budget runs out, or "returned false" would prove nothing.
  pid_t pid = 0;
  while (Clock::now() - started < std::chrono::milliseconds(4500)) {
    if (fs::exists(pidfile)) {
      pid = static_cast<pid_t>(std::atoi(slurp(pidfile).c_str()));
      if (pid > 0) break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  ASSERT_GT(pid, 0) << "the stand-in ffmpeg never started";
  EXPECT_TRUE(alive(pid));
  EXPECT_LT(Clock::now() - started, std::chrono::milliseconds(5000));

  // Bounded wait for the verdict: the probe budget is 5000 ms plus a 200 ms
  // SIGTERM grace; 5600 ms is the plan's ceiling. A probe with no timeout
  // would not answer for 120 s.
  ASSERT_EQ(call.wait_until(started + std::chrono::milliseconds(5600)), std::future_status::ready)
      << "the probe did not give up within its budget";
  EXPECT_FALSE(call.get());

  const auto reapDeadline = Clock::now() + std::chrono::milliseconds(1000);
  while (alive(pid) && Clock::now() < reapDeadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  EXPECT_FALSE(alive(pid)) << "the stubborn stand-in outlived the probe";
  if (alive(pid)) ::kill(pid, SIGKILL);
}

TEST(PosixEncoderProbe, PassesTheEncoderAndANullSink) {
  FakeFfmpeg fake;
  const fs::path argvFile = fake.dir / "argv.txt";
  fake.mode("argv-to " + argvFile.string());
  EXPECT_TRUE(posixFfmpegEncoderIsAvailable(fake.ffmpeg.string(), "hevc_videotoolbox"));
  // The exact argv, the same one the Windows probe sends (NUL -> "-").
  EXPECT_EQ(slurp(argvFile),
            "-hide_banner -loglevel quiet -nostdin -f lavfi -i color=c=black:s=1280x720:r=30 "
            "-frames:v 30 -an -c:v hevc_videotoolbox -f null -");
}

// Opt-in: COREVIDEO_TEST_FFMPEG names a real ffmpeg (the bundled LGPL build).
// Logs and returns green when unset (the vendored gtest has no GTEST_SKIP).
// Note: VideoToolbox can refuse a session while other encoders hold the media
// engine (observed: exit 187, OSStatus -12903, during a parallel AVF test run).
TEST(PosixEncoderProbe, BundledFfmpegEncodesWithBothVideoToolboxEncoders) {
  const char* ffmpeg = std::getenv("COREVIDEO_TEST_FFMPEG");
  if (ffmpeg == nullptr || *ffmpeg == '\0') {
    std::fprintf(stderr, "[ SKIPPED ] COREVIDEO_TEST_FFMPEG is not set\n");
    return;
  }
  EXPECT_TRUE(posixFfmpegEncoderIsAvailable(ffmpeg, "h264_videotoolbox"));
  EXPECT_TRUE(posixFfmpegEncoderIsAvailable(ffmpeg, "hevc_videotoolbox"));
  EXPECT_FALSE(posixFfmpegEncoderIsAvailable(ffmpeg, "libx264"));  // never in the LGPL build
}

}  // namespace

#endif  // !_WIN32
