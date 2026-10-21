#pragma once
#include <string>

namespace corevideo::core {
// Keep monitor dispatch, scene drawing and tile attribution on the same source
// identity conventions as the Program plan.
struct ResolvedMultiviewFeed {
  std::string kind;
  std::string sourceId;
  std::string participantId;
  std::string mediaAssetId;
};

inline ResolvedMultiviewFeed resolveMultiviewFeed(
    const std::string& srcKind,
    const std::string& sourceId,
    const std::string& participantId,
    const std::string& captureDeviceId,
    const std::string& mediaAssetId) {
  ResolvedMultiviewFeed feed;
  if (srcKind == "media" && !mediaAssetId.empty()) {
    feed.kind = "media-video";
    feed.sourceId = "media:" + mediaAssetId;
    feed.mediaAssetId = mediaAssetId;
  } else if (srcKind == "capture" && !captureDeviceId.empty()) {
    feed.kind = "participant-video";
    feed.participantId = "capture:" + captureDeviceId;
    feed.sourceId = feed.participantId;
  } else if (!participantId.empty()) {
    feed.kind = "participant-video";
    feed.participantId = participantId;
    feed.sourceId = "zoom:" + participantId;
  } else {
    feed.kind = "participant-video";
    feed.sourceId = sourceId;
  }
  return feed;
}
}  // namespace corevideo::core
