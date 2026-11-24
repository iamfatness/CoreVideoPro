// Does a finished recording give its hardware encoder session back?
//
// Found in Phase 4 Task 2: after the AVF recording suites ran, VideoToolbox
// refused every new hardware session (-12903) until the test process exited.
// The shipping core wraps the AVFoundation recorder in ONE AsyncEncoderSink for
// the whole process (ModuleComposition.cpp), so every recording opens and
// finalizes on that sink's process-lifetime writer thread. This test drives
// exactly that composition: many short recordings on one sink, then asks
// VideoToolbox for a hardware session. If finished recordings hold sessions, an
// operator's stream (or the next recording) fails after a few takes.
#if !COREVIDEO_STUB && COREVIDEO_ENABLE_DEV_ADAPTERS && COREVIDEO_WITH_AVF_ENCODER

#import <CoreMedia/CoreMedia.h>
#import <Foundation/Foundation.h>
#import <VideoToolbox/VideoToolbox.h>

#include <gtest/gtest.h>

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <unistd.h>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "modules/AsyncEncoderSink.h"
#include "modules/Interfaces.h"

namespace {
using namespace corevideo::modules;
namespace fs = std::filesystem;

OSStatus probeHardwareSession() {
  NSDictionary* spec = @{(id)kVTVideoEncoderSpecification_RequireHardwareAcceleratedVideoEncoder: @YES};
  VTCompressionSessionRef probe = nullptr;
  const OSStatus st = VTCompressionSessionCreate(kCFAllocatorDefault, 1920, 1080, kCMVideoCodecType_H264,
                                                 (__bridge CFDictionaryRef)spec, nullptr, nullptr, nullptr,
                                                 nullptr, &probe);
  if (probe) {
    VTCompressionSessionInvalidate(probe);
    CFRelease(probe);
  }
  return st;
}

ProgramFrame frame(int64_t n) {
  ProgramFrame f;
  f.frameNumber = n;
  f.width = 640;
  f.height = 360;
  f.programFullBgra.width = 640;
  f.programFullBgra.height = 360;
  f.programFullBgra.bgra.assign(640u * 360u * 4u, static_cast<uint8_t>(n * 7));
  return f;
}

TEST(AvfRecorderSessionRelease, FinishedRecordingsGiveTheirHardwareSessionsBack) {
  if (probeHardwareSession() != noErr) {
    std::fprintf(stderr, "[  SKIPPED ] AvfRecorderSessionRelease.FinishedRecordingsGiveTheirHardwareSessionsBack"
                         " (no hardware encoder session before the test) - this test did NOT run\n");
    return;
  }
  auto inner = createAVFoundationEncoderSink();
  if (!inner) {
    std::fprintf(stderr, "[  SKIPPED ] AvfRecorderSessionRelease (AVF encoder not built) - this test did NOT run\n");
    return;
  }
  // The production composition: one async sink for the life of the core.
  AsyncEncoderSink sink(std::move(inner));
  // Unique per process (several worktrees share $TMPDIR), removed on every exit path.
  const auto dir = fs::temp_directory_path() / ("avf-session-release-" + std::to_string(::getpid()));
  struct Cleanup {
    fs::path dir;
    ~Cleanup() {
      std::error_code ignored;
      fs::remove_all(dir, ignored);
    }
  } cleanup{dir};
  {
    std::error_code ignored;
    fs::remove_all(dir, ignored);
    fs::create_directories(dir, ignored);
  }
  const std::vector<float> pcm(960 * 2, 0.1f);
  constexpr int kRecordings = 40;  // well past the count that exhausted the media engine in-suite
  for (int take = 1; take <= kRecordings; ++take) {
    RecordingSessionRequest request;
    request.sessionId = "release-take-" + std::to_string(take);
    request.targetFolder = dir.string();
    request.filenamePrefix = "take" + std::to_string(take);
    request.format = "mp4";
    request.width = 640;
    request.height = 360;
    request.fps = 30;
    request.targetBitrateMbps = 4.0;
    request.audioBitrateKbps = 160;
    sink.configureRecording(request);
    sink.start({"recording"}, {});
    // The sink applies start on its writer thread: wait until it is recording,
    // or this loop measures nothing (it did, on first writing: 40 takes in 0.6 s).
    auto waitFor = [&](auto done, int seconds) {
      const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
      std::string status;
      while (std::chrono::steady_clock::now() < deadline) {
        status = sink.session().recordingStatus;
        if (done(status)) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
      }
      return status;
    };
    ASSERT_EQ(waitFor([](const std::string& s) { return s == "recording"; }, 10), "recording")
        << "take " << take << " never started";
    for (int i = 1; i <= 10; ++i) {
      sink.submit(frame(i));
      sink.submitAudio(pcm.data(), 960, 2, 48000);
    }
    sink.stopRecording();
    const std::string finished = waitFor(
        [](const std::string& s) { return s == "stopped" || s == "completed" || s == "failed" || s == "idle"; }, 15);
    ASSERT_EQ(finished, "stopped") << "take " << take << " did not finish cleanly";
    const auto session = sink.session();
    // Frames must really have been encoded, or no encoder session was ever
    // opened and a regression of the fix could not fail this test.
    ASSERT_GT(session.recordingVideoFrameCount, 0) << "take " << take << " encoded no video";
    ASSERT_FALSE(session.recordingArtifactPath.empty()) << "take " << take;
    std::error_code ec;
    const auto size = fs::file_size(session.recordingArtifactPath, ec);
    ASSERT_FALSE(ec) << "take " << take << " has no file: " << ec.message();
    ASSERT_GT(size, 0u) << "take " << take << " wrote an empty file";
    {  // every take: the failure names the exact take that exhausted the media engine
      const OSStatus st = probeHardwareSession();
      ASSERT_EQ(st, noErr) << "after " << take << " finished recordings VideoToolbox has no hardware session ("
                           << st << "): finished recordings are holding encoder sessions";
    }
  }
}

}  // namespace

#endif
