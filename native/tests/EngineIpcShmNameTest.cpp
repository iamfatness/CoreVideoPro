#include "engine-ipc.h"

#include <gtest/gtest.h>

#ifndef WIN32

#include <sys/mman.h>
#include <unistd.h>

#include <string>

namespace {

// Regression guard for the macOS PSHMNAMLEN limit: shm_open rejects names
// longer than 31 characters INCLUDING the leading '/', which is shorter than
// every logical name the core and engine build (IPC_SHM_PREFIX alone is 14).
// If this regresses, no SHM region can be opened on macOS and no Zoom frame
// is ever delivered.

TEST(EngineIpcShmName, FitsPlatformLimitAndKeepsLegacyNamesElsewhere) {
  const std::string video = IPC_SHM_PREFIX "source_1234567890123456_0";
  const std::string audio = video + "_audio";

  const std::string videoName = shm_platform_name(video);
  const std::string audioName = shm_platform_name(audio);

#ifdef __APPLE__
  // Only Apple collapses the name: PSHMNAMLEN caps shm names at 31 bytes
  // there. Other platforms deliberately keep the legacy long name so the
  // wire format never changes for Windows/Linux.
  EXPECT_LE(videoName.size(), 31u);
  EXPECT_LE(audioName.size(), 31u);
#else
  EXPECT_EQ(videoName, "/" + video);
  EXPECT_EQ(audioName, "/" + audio);
#endif
  ASSERT_FALSE(videoName.empty());
  ASSERT_FALSE(audioName.empty());
  EXPECT_EQ(videoName[0], '/');
  EXPECT_EQ(audioName[0], '/');

  // Deterministic: both sides derive the name independently and must agree.
  EXPECT_EQ(shm_platform_name(video), videoName);

  // Distinct logical names must not collide onto one region, or the audio
  // and video paths would trample each other.
  EXPECT_NE(videoName, audioName);
  EXPECT_NE(shm_platform_name(IPC_SHM_PREFIX "source_1234567890123456_1"), videoName);
}

TEST(EngineIpcShmName, CreateAndOpenResolveTheSameRegion) {
  const std::string video = IPC_SHM_PREFIX "source_1234567890123456_0";

  ShmRegion writer;
  ASSERT_TRUE(shm_region_create(writer, video, 4096));
  ShmRegion reader;
  EXPECT_TRUE(shm_region_open_read(reader, video, 4096));
  shm_region_destroy(reader);
  shm_region_destroy(writer);
}

TEST(EngineIpcShmName, CreateOverLeakedRegionSucceedsAtLargerSize) {
  // A POSIX shm object can be sized exactly once, so a region left behind by
  // a crashed engine could never be resized and every subsequent create
  // against that name failed (macOS ftruncate -> EINVAL). Simulate the leak
  // by creating a region and dropping the handle WITHOUT unlinking, the way
  // a killed process does, then create again at a larger size.
  const std::string video = IPC_SHM_PREFIX "source_1234567890123456_0";

  ShmRegion leaked;
  ASSERT_TRUE(shm_region_create(leaked, video, 4096));
  // Abandon the name exactly as a killed process would: close and unmap,
  // but never shm_unlink.
  if (leaked.ptr) munmap(leaked.ptr, leaked.size);
  if (leaked.fd >= 0) close(leaked.fd);
  leaked = ShmRegion{};

  ShmRegion grown;
  EXPECT_TRUE(shm_region_create(grown, video, 65536));
  shm_region_destroy(grown);
}

}  // namespace

#endif  // !WIN32
