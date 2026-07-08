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
#include "modules/VirtualCameraShm.h"

using corevideo::modules::VirtualCameraShmHeader;
using corevideo::modules::kVirtualCameraMagic;
using corevideo::modules::virtualCameraShmName;
using corevideo::modules::virtualCameraShmSize;
using corevideo::virtualcam::SharedFrameReader;

namespace {

// Minimal writer mirroring WindowsVirtualCameraPublisher's slot format.
struct ShmWriter {
  HANDLE mapping = nullptr;
  void* view = nullptr;
  VirtualCameraShmHeader* header = nullptr;

  bool open() {
    mapping = CreateFileMappingA(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0,
                                 static_cast<DWORD>(virtualCameraShmSize()),
                                 virtualCameraShmName().c_str());
    if (mapping == nullptr) return false;
    view = MapViewOfFile(mapping, FILE_MAP_WRITE, 0, 0, virtualCameraShmSize());
    if (view == nullptr) return false;
    header = static_cast<VirtualCameraShmHeader*>(view);
    header->seq = 0;
    header->magic = kVirtualCameraMagic;
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

  ~ShmWriter() {
    if (view) UnmapViewOfFile(view);
    if (mapping) CloseHandle(mapping);
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

#endif  // _WIN32
