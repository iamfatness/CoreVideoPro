#pragma once
// Test helper: decode an MP4's first video track to BGRA with AVAssetReader.
// Implemented in AvfMp4Decode.mm (Apple only).
#include <cstdint>
#include <string>
#include <vector>

namespace corevideo::testing {

struct DecodedMp4Video {
  int width = 0;   // track natural size
  int height = 0;
  std::vector<std::vector<uint8_t>> frames;  // tightly packed BGRA, width*height*4 each
  std::string error;
};

DecodedMp4Video decodeMp4VideoToBgra(const std::string& path);

// Longest track duration in seconds (0 if the file is unreadable).
double mp4DurationSeconds(const std::string& path);

// Per-track sample timing read straight from the container (seconds). `end` is
// the last sample's PTS plus its duration, i.e. where that track stops.
struct Mp4TrackExtent {
  bool present = false;
  size_t samples = 0;
  double firstPts = 0, lastPts = 0, end = 0;
  double trackEnd = 0;  // the track's own timeRange end (what a player seeks to)
};
struct Mp4Extents { Mp4TrackExtent video, audio; };
Mp4Extents mp4TrackExtents(const std::string& path);

}  // namespace corevideo::testing
