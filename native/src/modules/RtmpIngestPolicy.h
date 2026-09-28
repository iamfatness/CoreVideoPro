#pragma once

#include <algorithm>
#include <cctype>
#include <string>
#include <vector>

#include "modules/SrtFfmpegArgs.h"

namespace corevideo::modules {

// RTMP contribution arrives at a local listener. A bare stream name is enough
// for FFmpeg to serve one publisher; the app never needs a remote pull server.
inline std::string validateRtmpIngestUrl(const std::string& url) {
  if (url.rfind("rtmp://", 0) != 0) {
    return "RTMP ingest needs an rtmp:// listener URL.";
  }
  const auto hostStart = std::string("rtmp://").size();
  const auto pathStart = url.find('/', hostStart);
  const auto authority = pathStart == std::string::npos
      ? std::string() : url.substr(hostStart, pathStart - hostStart);
  if (pathStart == std::string::npos || pathStart == hostStart ||
      pathStart + 1 >= url.size() || url.find('/', pathStart + 1) == std::string::npos ||
      url.back() == '/' || authority.empty() || authority.front() == ':') {
    return "RTMP ingest URL needs a host, application, and stream name.";
  }
  if (authority.find('@') != std::string::npos ||
      url.find_first_of("?#\"\\") != std::string::npos ||
      std::any_of(url.begin(), url.end(), [](unsigned char ch) { return std::isspace(ch) != 0; })) {
    return "RTMP ingest URL cannot contain credentials, query, fragment, or whitespace.";
  }
  return {};
}

inline std::string redactedRtmpIngestUrl(const std::string& url) {
  const auto hostStart = url.find("://");
  if (hostStart == std::string::npos) return "<rtmp-source>";
  const auto pathStart = url.find('/', hostStart + 3);
  if (pathStart == std::string::npos) return "<rtmp-source>";
  return url.substr(0, pathStart) + "/<rtmp-source>";
}

inline std::vector<std::string> buildRtmpIngestArgv(const std::string& executable,
                                                    const std::string& url,
                                                    int width, int height, int frameRate,
                                                    const std::string& audioSink = {}) {
  // FFmpeg accepts one publisher on this URL. The stream path can act as a
  // credential, so suppress its unredacted stderr until a scrubbed diagnostic
  // channel exists; source health still reports startup and frame loss.
  return buildSrtIngestArgv(executable, url, width, height, frameRate,
                            audioSink, {"-listen", "1"}, "quiet");
}


}  // namespace corevideo::modules
