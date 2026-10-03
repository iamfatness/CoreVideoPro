#include "modules/FfmpegChildRetirement.h"
#include "modules/Interfaces.h"

#include <gtest/gtest.h>

#ifdef _WIN32
#include <string>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <thread>

namespace {

HANDLE startStubbornChild() {
  STARTUPINFOA startup{};
  startup.cb = sizeof(startup);
  PROCESS_INFORMATION child{};
  if (!::CreateProcessA(COREVIDEO_STUBBORN_FFMPEG_PATH, nullptr, nullptr, nullptr, FALSE,
                        CREATE_NO_WINDOW, nullptr, nullptr, &startup, &child)) {
    return nullptr;
  }
  ::CloseHandle(child.hThread);
  return child.hProcess;
}

TEST(FfmpegChildRetirement, StopReapsUncooperativeChildBeforeReplacementStarts) {
  HANDLE oldChild = startStubbornChild();
  ASSERT_NE(oldChild, nullptr) << "could not start the test child: " << ::GetLastError();
  ASSERT_EQ(::WaitForSingleObject(oldChild, 0), WAIT_TIMEOUT);

  // The child never reads stdin. The graceful timeout therefore expires, just
  // like an egress FFmpeg wedged on a slow destination.
  ASSERT_TRUE(corevideo::modules::retireFfmpegChild(oldChild, 20));
  EXPECT_EQ(::WaitForSingleObject(oldChild, 0), WAIT_OBJECT_0)
      << "a replacement would overlap a still-publishing child";

  HANDLE replacement = startStubbornChild();
  ASSERT_NE(replacement, nullptr);
  EXPECT_EQ(::WaitForSingleObject(oldChild, 0), WAIT_OBJECT_0);
  EXPECT_EQ(::WaitForSingleObject(replacement, 0), WAIT_TIMEOUT);
  EXPECT_TRUE(corevideo::modules::retireFfmpegChild(replacement, 20));
  ::CloseHandle(replacement);
  ::CloseHandle(oldChild);
}

TEST(FfmpegChildRetirement, SenderStopReapsItsActualTransportChild) {
#if COREVIDEO_WITH_RTMP_OUTPUT
  namespace fs = std::filesystem;
  const fs::path folder = fs::temp_directory_path() /
      ("corevideo-stop-child-" + std::to_string(::GetCurrentProcessId()));
  std::error_code error;
  fs::create_directories(folder, error);
  ASSERT_FALSE(error) << error.message();
  const fs::path ffmpeg = folder / "ffmpeg.exe";
  const fs::path marker = folder / "child.pid";
  fs::remove(marker, error);
  fs::copy_file(COREVIDEO_STUBBORN_FFMPEG_PATH, ffmpeg,
                fs::copy_options::overwrite_existing, error);
  ASSERT_FALSE(error) << error.message();
  ASSERT_TRUE(::SetEnvironmentVariableA("COREVIDEO_TEST_FFMPEG_PID_FILE", marker.string().c_str()));

  auto sender = corevideo::modules::createRtmpOutputSender();
  ASSERT_NE(sender, nullptr);
  corevideo::modules::OutputDestinationSettings settings;
  settings.id = "rtmp";
  settings.label = "RTMP";
  settings.protocol = "rtmp";
  settings.url = "rtmp://127.0.0.1:1/live";
  settings.streamKey = "stop-child";
  settings.ffmpegBinDirectory = folder.string();
  settings.videoCodec = "h264";
  corevideo::modules::ProgramFrame frame{16, 16, 2, 7, "stop-child", "d3d11"};
  frame.programFullBgra.width = 16;
  frame.programFullBgra.height = 16;
  frame.programFullBgra.bgra.assign(16u * 16u * 4u, 0x10);
  const auto opened = sender->sync({"rtmp"}, &frame, 0, {settings});
  ASSERT_FALSE(opened.senders.empty());

  // #754: a wait bound, not a budget. These tests condition-wait and only this
  // bound fails them, so it is sized for a loaded shared CI runner, not for the
  // quiet dev box where 2-3 s always sufficed.
  constexpr auto kWaitBound = std::chrono::seconds(60);
  const auto deadline = std::chrono::steady_clock::now() + kWaitBound;
  while (!fs::exists(marker) && std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  DWORD childPid = 0;
  if (fs::exists(marker)) {
    std::ifstream input(marker);
    input >> childPid;
  }
  HANDLE child = childPid ? ::OpenProcess(SYNCHRONIZE | PROCESS_TERMINATE, FALSE, childPid) : nullptr;
  EXPECT_NE(child, nullptr) << "the sender never launched the stubborn transport child";
  if (child) EXPECT_EQ(::WaitForSingleObject(child, 0), WAIT_TIMEOUT);

  sender->sync({}, nullptr, 1);
  if (child) {
    const DWORD stopped = ::WaitForSingleObject(child, 0);
    EXPECT_EQ(stopped, WAIT_OBJECT_0) << "the sender returned Stop while its old child was still alive";
    if (stopped != WAIT_OBJECT_0) {
      ::TerminateProcess(child, 1);
      ::WaitForSingleObject(child, 1000);
    }
  }
  fs::remove(marker, error);
  sender->sync({"rtmp"}, &frame, 2, {settings});
  const auto restartDeadline = std::chrono::steady_clock::now() + kWaitBound;
  while (!fs::exists(marker) && std::chrono::steady_clock::now() < restartDeadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  DWORD replacementPid = 0;
  if (fs::exists(marker)) {
    std::ifstream input(marker);
    input >> replacementPid;
  }
  EXPECT_NE(replacementPid, 0u) << "restart did not launch a replacement child";
  EXPECT_NE(replacementPid, childPid);
  if (child) EXPECT_EQ(::WaitForSingleObject(child, 0), WAIT_OBJECT_0);
  sender->sync({}, nullptr, 3);
  if (child) ::CloseHandle(child);
  sender.reset();
  ::SetEnvironmentVariableA("COREVIDEO_TEST_FFMPEG_PID_FILE", nullptr);
  fs::remove(marker, error);
  fs::remove(ffmpeg, error);
  fs::remove(folder, error);
#endif
}

}  // namespace
#endif
