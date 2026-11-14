// POSIX ffmpeg discovery (modules/PosixExecutableLocator.h). Compiled into the
// test target on non-Windows builds only (native/CMakeLists.txt).
#include "modules/PosixExecutableLocator.h"

#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>

#include <sys/stat.h>
#include <unistd.h>

namespace fs = std::filesystem;
using corevideo::modules::posix_exec::currentExecutableDirectory;
using corevideo::modules::posix_exec::defaultFfmpegFallbackDirectories;
using corevideo::modules::posix_exec::findOnPathThenFallbacks;
using corevideo::modules::posix_exec::isExecutableFile;

namespace {

class TempDir {
 public:
  TempDir() {
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    path_ = fs::temp_directory_path() /
            ("cv-exec-locator-" + std::to_string(::getpid()) + "-" + std::to_string(stamp) + "-" +
             std::to_string(counter_++));
    fs::create_directories(path_);
  }
  ~TempDir() {
    std::error_code ignored;
    fs::remove_all(path_, ignored);
  }
  const fs::path& path() const { return path_; }

 private:
  static inline int counter_ = 0;
  fs::path path_;
};

fs::path writeFile(const fs::path& dir, const std::string& name, bool executable) {
  const auto file = dir / name;
  std::ofstream(file) << "#!/bin/sh\nexit 0\n";
  ::chmod(file.c_str(), executable ? 0755 : 0644);
  return file;
}

}  // namespace

TEST(PosixExecutableLocator, FindsFfmpegInAFallbackDirWhenPathLacksIt) {
  TempDir onPath, fallback;
  const auto ffmpeg = writeFile(fallback.path(), "ffmpeg", true);
  // PATH = a dir without ffmpeg (what a Finder launch looks like for Homebrew).
  const std::string pathEnv = onPath.path().string();
  EXPECT_EQ(findOnPathThenFallbacks("ffmpeg", pathEnv.c_str(), {fallback.path()}), ffmpeg.string());
  EXPECT_EQ(findOnPathThenFallbacks("ffmpeg", nullptr, {fallback.path()}), ffmpeg.string());
}

TEST(PosixExecutableLocator, PathStillWinsOverTheFallbackList) {
  TempDir onPath, fallback;
  const auto fromPath = writeFile(onPath.path(), "ffmpeg", true);
  writeFile(fallback.path(), "ffmpeg", true);
  const std::string pathEnv = "/nonexistent-cv-dir:" + onPath.path().string();
  EXPECT_EQ(findOnPathThenFallbacks("ffmpeg", pathEnv.c_str(), {fallback.path()}), fromPath.string());
}

TEST(PosixExecutableLocator, SkipsANonExecutableFileOnPathAndInFallbacks) {
  TempDir onPath, stale, good;
  writeFile(onPath.path(), "ffmpeg", false);
  writeFile(stale.path(), "ffmpeg", false);
  const auto usable = writeFile(good.path(), "ffmpeg", true);
  const std::string pathEnv = onPath.path().string();
  EXPECT_FALSE(isExecutableFile(onPath.path() / "ffmpeg"));
  EXPECT_EQ(findOnPathThenFallbacks("ffmpeg", pathEnv.c_str(), {stale.path(), good.path()}),
            usable.string());
  EXPECT_EQ(findOnPathThenFallbacks("ffmpeg", pathEnv.c_str(), {stale.path()}), "");
}

TEST(PosixExecutableLocator, SkipsADirectoryNamedLikeTheExecutable) {
  TempDir dir;
  fs::create_directories(dir.path() / "ffmpeg");
  EXPECT_EQ(findOnPathThenFallbacks("ffmpeg", dir.path().c_str(), {}), "");
}

TEST(PosixExecutableLocator, FollowsASymlinkLikeHomebrewsBinEntries) {
  TempDir cellar, bin;
  const auto real = writeFile(cellar.path(), "ffmpeg-real", true);
  fs::create_symlink(real, bin.path() / "ffmpeg");
  EXPECT_EQ(findOnPathThenFallbacks("ffmpeg", nullptr, {bin.path()}), (bin.path() / "ffmpeg").string());
}

TEST(PosixExecutableLocator, EmptyPathEntriesAreIgnoredNotTreatedAsCwd) {
  TempDir fallback;
  const auto ffmpeg = writeFile(fallback.path(), "ffmpeg", true);
  EXPECT_EQ(findOnPathThenFallbacks("ffmpeg", "::", {fallback.path()}), ffmpeg.string());
}

TEST(PosixExecutableLocator, DefaultFallbacksNameHomebrewAndMacPorts) {
  const auto dirs = defaultFfmpegFallbackDirectories();
  ASSERT_EQ(dirs.size(), 3u);
  EXPECT_EQ(dirs[0], fs::path("/opt/homebrew/bin"));
  EXPECT_EQ(dirs[1], fs::path("/usr/local/bin"));
  EXPECT_EQ(dirs[2], fs::path("/opt/local/bin"));
}

TEST(PosixExecutableLocator, CurrentExecutableDirectoryIsRealAndHoldsThisBinary) {
  const auto dir = currentExecutableDirectory();
  ASSERT_FALSE(dir.empty());
  EXPECT_TRUE(dir.is_absolute());
  EXPECT_TRUE(fs::exists(dir / "corevideo-native-tests")) << dir;
}
