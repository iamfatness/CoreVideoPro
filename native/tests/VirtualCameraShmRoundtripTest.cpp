// Verifies the core<->DLL shared-memory contract in-process: write an NV12 frame
// into the slot exactly as the publisher does (seqlock), then read it back with
// the DLL's SharedFrameReader. This covers the SHM layout + seqlock discipline
// that is otherwise only exercised when the Frame Server loads the DLL for real.
#include <gtest/gtest.h>

#if defined(_WIN32)
#include <windows.h>

#include <cstring>
#include <vector>

#include "SharedFrameReader.h"  // native/virtualcam-dll (on the test include path)
#include "modules/VirtualCameraPublisher.h"
#include "modules/VirtualCameraShm.h"

using corevideo::modules::VirtualCameraShmHeader;
using corevideo::modules::kVirtualCameraMagic;
using corevideo::modules::mapVirtualCameraShmView;
using corevideo::modules::openVirtualCameraShmFile;
using corevideo::modules::virtualCameraShmFilePath;
using corevideo::modules::virtualCameraShmSize;
using corevideo::virtualcam::SharedFrameReader;

namespace {

// Minimal writer mirroring WindowsVirtualCameraPublisher's slot format
// (file-backed on %ProgramData%, exactly as the real publisher does it).
struct ShmWriter {
  HANDLE file = INVALID_HANDLE_VALUE;
  HANDLE mapping = nullptr;
  void* view = nullptr;
  VirtualCameraShmHeader* header = nullptr;
  bool deleteOnClose = true;

  bool open() {
    file = openVirtualCameraShmFile(/*writer=*/true);
    if (file == INVALID_HANDLE_VALUE) return false;
    view = mapVirtualCameraShmView(file, /*writer=*/true, &mapping);
    if (view == nullptr) return false;
    header = static_cast<VirtualCameraShmHeader*>(view);
    // Mirror WindowsVirtualCameraPublisher::start(): the file is REUSED across
    // runs, so re-init under the seqlock, continuing FORWARD from any prior seq
    // (never back to 0 - a reader from the prior run must see a CHANGED seq).
    header->seq = header->seq | 1u;  // odd: writing
    header->byteLen = 0;
    header->frameNumber = 0;
    header->magic = kVirtualCameraMagic;
    header->seq = (header->seq | 1u) + 1u;  // even = complete
    return true;
  }

  void write(const std::vector<std::uint8_t>& nv12, int w, int h) {
    auto* payload = static_cast<std::uint8_t*>(view) + sizeof(VirtualCameraShmHeader);
    header->seq = header->seq + 1;  // odd
    std::memcpy(payload, nv12.data(), nv12.size());
    header->width = w;
    header->height = h;
    header->byteLen = static_cast<std::uint32_t>(nv12.size());
    header->frameNumber += 1;
    header->seq = header->seq + 1;  // even
  }

  void closeHandles() {
    if (view) UnmapViewOfFile(view);
    if (mapping) CloseHandle(mapping);
    if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
    view = nullptr;
    mapping = nullptr;
    file = INVALID_HANDLE_VALUE;
    header = nullptr;
  }

  ~ShmWriter() {
    closeHandles();
    if (deleteOnClose) {
      ::DeleteFileA(virtualCameraShmFilePath().c_str());  // no stale slot for the next test
    }
  }
};

}  // namespace

TEST(VirtualCameraShmRoundtrip, ReadsBackTheFrameTheWriterPublished) {
  ShmWriter writer;
  ASSERT_TRUE(writer.open());

  const int w = 64, h = 36;
  std::vector<std::uint8_t> frame(static_cast<size_t>(w) * h * 3 / 2);
  for (size_t i = 0; i < frame.size(); ++i) {
    frame[i] = static_cast<std::uint8_t>(i * 7 + 3);  // deterministic pattern
  }
  writer.write(frame, w, h);

  SharedFrameReader reader;
  std::vector<std::uint8_t> out;
  int rw = 0, rh = 0;
  ASSERT_TRUE(reader.readLatest(out, rw, rh));
  EXPECT_EQ(rw, w);
  EXPECT_EQ(rh, h);
  EXPECT_EQ(out, frame);
}

TEST(VirtualCameraShmRoundtrip, NoRegionMeansNoFrame) {
  // With no writer mapping alive, the reader reports "no frame" (not a crash).
  ::DeleteFileA(virtualCameraShmFilePath().c_str());  // ensure no stale slot file
  SharedFrameReader reader;
  std::vector<std::uint8_t> out;
  int rw = 0, rh = 0;
  EXPECT_FALSE(reader.readLatest(out, rw, rh));
}

TEST(VirtualCameraShmRoundtrip, LatestWriteWins) {
  ShmWriter writer;
  ASSERT_TRUE(writer.open());
  const int w = 8, h = 8;
  std::vector<std::uint8_t> a(static_cast<size_t>(w) * h * 3 / 2, 0x11);
  std::vector<std::uint8_t> b(static_cast<size_t>(w) * h * 3 / 2, 0x22);
  writer.write(a, w, h);
  writer.write(b, w, h);  // newest

  SharedFrameReader reader;
  std::vector<std::uint8_t> out;
  int rw = 0, rh = 0;
  ASSERT_TRUE(reader.readLatest(out, rw, rh));
  EXPECT_EQ(out, b);
}

// REGRESSION (the "flashing camera"): the writer restarting must NOT orphan a
// reader that already holds the backing file. The old writer deleted+recreated
// the path on start; a Frame Server reader kept mapping the unlinked old file
// object (FILE_SHARE_DELETE), never saw another frame, and served the slate
// forever - interleaved with a fresh instance's program frames = strobing.
TEST(VirtualCameraShmRoundtrip, WriterRestartKeepsAnExistingReaderLive) {
  const int w = 8, h = 8;
  std::vector<std::uint8_t> f1(static_cast<size_t>(w) * h * 3 / 2, 0x11);
  std::vector<std::uint8_t> f2(static_cast<size_t>(w) * h * 3 / 2, 0x22);

  SharedFrameReader reader;
  std::vector<std::uint8_t> out;
  int rw = 0, rh = 0;
  {
    ShmWriter first;
    first.deleteOnClose = false;  // an app restart does not remove the slot file
    ASSERT_TRUE(first.open());
    first.write(f1, w, h);
    ASSERT_TRUE(reader.readLatest(out, rw, rh));
    EXPECT_EQ(out, f1);
  }  // "app exit": writer handles closed, file stays

  ShmWriter second;  // "app relaunch": open-or-create IN PLACE (same file object)
  ASSERT_TRUE(second.open());
  second.write(f2, w, h);

  // The ORIGINAL reader mapping (opened before the restart) must see the new
  // frame - with a delete-on-start writer this read returned stale/no frames.
  ASSERT_TRUE(reader.readLatest(out, rw, rh));
  EXPECT_EQ(out, f2);
}

// If the file object DOES get swapped underneath a reader (the exact failure
// mode the delete-on-start writer caused), the reader self-heals: after
// kReopenAfterUnchangedReads frozen requests it re-opens by path and serves the
// live file instead of degrading to the slate forever.
TEST(VirtualCameraShmRoundtrip, ReaderSelfHealsAfterTheBackingFileIsSwapped) {
  const int w = 8, h = 8;
  std::vector<std::uint8_t> f1(static_cast<size_t>(w) * h * 3 / 2, 0x11);
  std::vector<std::uint8_t> f2(static_cast<size_t>(w) * h * 3 / 2, 0x22);

  SharedFrameReader reader;
  std::vector<std::uint8_t> out;
  int rw = 0, rh = 0;
  {
    ShmWriter first;
    first.deleteOnClose = false;
    ASSERT_TRUE(first.open());
    first.write(f1, w, h);
    ASSERT_TRUE(reader.readLatest(out, rw, rh));
    EXPECT_EQ(out, f1);
    first.closeHandles();
    ::DeleteFileA(virtualCameraShmFilePath().c_str());  // swap: unlink the path
  }

  ShmWriter second;  // NEW file object at the same path
  ASSERT_TRUE(second.open());
  second.write(f2, w, h);
  second.write(f2, w, h);  // advance seq past any collision with the reader's last

  bool healed = false;
  for (std::uint32_t i = 0; i <= SharedFrameReader::kReopenAfterUnchangedReads + 5 && !healed;
       ++i) {
    healed = reader.readLatest(out, rw, rh);
  }
  ASSERT_TRUE(healed);
  EXPECT_EQ(out, f2);
}

#if defined(COREVIDEO_WITH_VIRTUALCAM) && COREVIDEO_WITH_VIRTUALCAM
// REGRESSION (G4 teardown finding): the REAL publisher's stop() must leave the
// slot file in place. Deleting it orphans Frame Server readers that hold the
// file object via FILE_SHARE_DELETE - they freeze on the unlinked file and
// interleave with the next session's fresh instance as program/slate strobing.
// start() may fail at MFCreateVirtualCamera on rigs without the registered DLL;
// the SHM slot is created before that step either way, so the assertion holds
// in both environments.
TEST(VirtualCameraPublisher, StopLeavesTheShmSlotFileInPlace) {
  auto publisher = corevideo::modules::createVirtualCameraPublisher();
  ASSERT_NE(publisher, nullptr);
  publisher->start(1920, 1080, 60);
  if (::GetFileAttributesA(virtualCameraShmFilePath().c_str()) ==
      INVALID_FILE_ATTRIBUTES) {
    // Slot could not be created (no ProgramData access on this rig) - there is
    // nothing to assert about stop() then. (This gtest has no GTEST_SKIP.)
    return;
  }
  publisher->stop();
  EXPECT_NE(::GetFileAttributesA(virtualCameraShmFilePath().c_str()),
            INVALID_FILE_ATTRIBUTES)
      << "stop() must NOT delete the vcam SHM file: readers hold it via "
         "FILE_SHARE_DELETE and delete orphans them (frozen frames / strobing)";
  ::DeleteFileA(virtualCameraShmFilePath().c_str());  // test isolation only
}
#endif  // COREVIDEO_WITH_VIRTUALCAM

#endif  // _WIN32
