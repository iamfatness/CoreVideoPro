#include "zoom/I420Convert.h"
#include "zoom/ShmFrameReader.h"

#include <gtest/gtest.h>

#include <cstring>
#include <vector>

namespace {

using corevideo::zoom::DecodedI420Frame;
using corevideo::zoom::FrameReadStatus;
using corevideo::zoom::i420ToRgbaThumbnail;
using corevideo::zoom::tryReadFrame;

void writeU32(std::vector<uint8_t>& buf, std::size_t offset, uint32_t value) {
  std::memcpy(buf.data() + offset, &value, sizeof(value));
}

// Build a shared-memory snapshot: header (seq,w,h,y_len) + Y + U + V planes.
std::vector<uint8_t> makeFrameBuffer(uint32_t sequence, uint32_t w, uint32_t h, uint8_t fill = 0) {
  const uint32_t yLen = w * h;
  const std::size_t planeBytes = yLen + yLen / 2u;
  std::vector<uint8_t> buf(corevideo::zoom::kShmFrameHeaderBytes + planeBytes, fill);
  writeU32(buf, 0, sequence);
  writeU32(buf, 4, w);
  writeU32(buf, 8, h);
  writeU32(buf, 12, yLen);
  return buf;
}

TEST(ShmFrameReader, DecodesCompleteEvenSequenceFrame) {
  auto buf = makeFrameBuffer(/*sequence=*/4, /*w=*/4, /*h=*/2, /*fill=*/7);
  DecodedI420Frame frame;
  EXPECT_EQ(tryReadFrame(buf.data(), buf.size(), frame), FrameReadStatus::Ok);
  EXPECT_EQ(frame.sequence, 4u);
  EXPECT_EQ(frame.width, 4u);
  EXPECT_EQ(frame.height, 2u);
  EXPECT_EQ(frame.i420.size(), 4u * 2u + (4u * 2u) / 2u);
  EXPECT_EQ(frame.i420.front(), 7u);
}

TEST(ShmFrameReader, ReportsInProgressForOddSequence) {
  auto buf = makeFrameBuffer(/*sequence=*/5, 4, 2);
  DecodedI420Frame frame;
  EXPECT_EQ(tryReadFrame(buf.data(), buf.size(), frame), FrameReadStatus::InProgress);
}

TEST(ShmFrameReader, ReportsEmptyForZeroDimensions) {
  auto buf = makeFrameBuffer(/*sequence=*/2, 0, 0);
  DecodedI420Frame frame;
  EXPECT_EQ(tryReadFrame(buf.data(), buf.size(), frame), FrameReadStatus::Empty);
}

TEST(ShmFrameReader, ReportsMalformedWhenPlanesTruncated) {
  auto buf = makeFrameBuffer(/*sequence=*/2, 8, 8);
  buf.resize(corevideo::zoom::kShmFrameHeaderBytes + 10);  // drop most plane bytes
  DecodedI420Frame frame;
  EXPECT_EQ(tryReadFrame(buf.data(), buf.size(), frame), FrameReadStatus::Malformed);
}

TEST(ShmFrameReader, ReportsMalformedForOddI420Dimensions) {
  auto buf = makeFrameBuffer(/*sequence=*/2, 6, 2);
  writeU32(buf, 8, 3);
  writeU32(buf, 12, 18);
  buf.resize(corevideo::zoom::kShmFrameHeaderBytes + 18 + 18 / 2u);
  DecodedI420Frame frame;
  EXPECT_EQ(tryReadFrame(buf.data(), buf.size(), frame), FrameReadStatus::Malformed);
}

TEST(ShmFrameReader, ReportsMalformedForUndersizedBuffer) {
  std::vector<uint8_t> buf(4, 0);
  DecodedI420Frame frame;
  EXPECT_EQ(tryReadFrame(buf.data(), buf.size(), frame), FrameReadStatus::Malformed);
  EXPECT_EQ(tryReadFrame(nullptr, 0, frame), FrameReadStatus::Malformed);
}

TEST(I420Convert, ConvertsSolidBlackToOpaqueRgba) {
  // I420 black is Y=16, U=V=128.
  const uint32_t w = 2, h = 2;
  std::vector<uint8_t> i420(w * h + (w * h) / 2u);
  std::fill(i420.begin(), i420.begin() + w * h, uint8_t{16});
  std::fill(i420.begin() + w * h, i420.end(), uint8_t{128});

  auto rgba = i420ToRgbaThumbnail(i420.data(), w, h, w, h);
  ASSERT_TRUE((rgba.size()) == (static_cast<std::size_t>(w) * h * 4u));
  EXPECT_TRUE((rgba[0]) <= (2u));  // ~black
  EXPECT_TRUE((rgba[1]) <= (2u));
  EXPECT_TRUE((rgba[2]) <= (2u));
  EXPECT_EQ(rgba[3], 255u);  // opaque
}

TEST(I420Convert, ConvertsSolidWhiteToOpaqueRgba) {
  const uint32_t w = 2, h = 2;
  std::vector<uint8_t> i420(w * h + (w * h) / 2u);
  std::fill(i420.begin(), i420.begin() + w * h, uint8_t{235});  // Y white
  std::fill(i420.begin() + w * h, i420.end(), uint8_t{128});

  auto rgba = i420ToRgbaThumbnail(i420.data(), w, h, w, h);
  EXPECT_GE(rgba[0], 250u);
  EXPECT_GE(rgba[1], 250u);
  EXPECT_GE(rgba[2], 250u);
  EXPECT_EQ(rgba[3], 255u);
}

TEST(I420Convert, ProducesRequestedThumbnailSize) {
  const uint32_t w = 16, h = 16;
  std::vector<uint8_t> i420(w * h + (w * h) / 2u, 128);
  std::fill(i420.begin(), i420.begin() + w * h, uint8_t{120});

  auto rgba = i420ToRgbaThumbnail(i420.data(), w, h, 4, 4);
  EXPECT_EQ(rgba.size(), 4u * 4u * 4u);
}

TEST(I420Convert, ReturnsEmptyForDegenerateInput) {
  std::vector<uint8_t> i420(6, 0);
  EXPECT_TRUE(i420ToRgbaThumbnail(i420.data(), 0, 0, 4, 4).empty());
  EXPECT_TRUE(i420ToRgbaThumbnail(nullptr, 2, 2, 2, 2).empty());
}

// End-to-end: decode a shared-memory frame, then thumbnail it.
TEST(ZoomFramePath, DecodeThenThumbnail) {
  auto buf = makeFrameBuffer(/*sequence=*/8, /*w=*/8, /*h=*/8, /*fill=*/128);
  DecodedI420Frame frame;
  ASSERT_TRUE((tryReadFrame(buf.data(), buf.size(), frame)) == (FrameReadStatus::Ok));
  auto rgba = i420ToRgbaThumbnail(frame.i420.data(), frame.width, frame.height, 4, 4);
  EXPECT_EQ(rgba.size(), 4u * 4u * 4u);
}

}  // namespace
