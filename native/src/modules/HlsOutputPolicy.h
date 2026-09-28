#pragma once

#include <algorithm>
#include <cctype>
#include <string>

namespace corevideo::modules {

// The HLS destination is an HTTP origin that accepts PUT for both playlists
// and segments. Reject userinfo and query tokens; the path may still be an
// opaque origin identifier, so diagnostics redact it separately.
inline std::string validateHlsPlaylistUrl(const std::string& url) {
  const auto schemeEnd = url.find("://");
  std::string scheme = schemeEnd == std::string::npos ? std::string() : url.substr(0, schemeEnd);
  std::transform(scheme.begin(), scheme.end(), scheme.begin(),
                 [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
  if (scheme != "http" && scheme != "https") {
    return "HLS output needs an http:// or https:// playlist URL.";
  }
  const auto hostStart = schemeEnd + 3;
  const auto pathStart = url.find('/', hostStart);
  if (pathStart == std::string::npos || pathStart == hostStart ||
      url.substr(hostStart, pathStart - hostStart).find('@') != std::string::npos) {
    return "HLS playlist URL needs a host and path, without embedded credentials.";
  }
  if (url.find_first_of("?#\"\\") != std::string::npos ||
      std::any_of(url.begin(), url.end(), [](unsigned char ch) { return std::isspace(ch) != 0; }) ||
      url.size() < 6 || url.substr(url.size() - 5) != ".m3u8") {
    return "HLS playlist URL must end in .m3u8 and contain no query, fragment, or whitespace.";
  }
  return {};
}

inline std::string hlsSegmentUrl(const std::string& playlistUrl) {
  const auto slash = playlistUrl.rfind('/');
  const auto stem = playlistUrl.substr(slash + 1, playlistUrl.size() - slash - 6);
  return playlistUrl.substr(0, slash + 1) + stem + "_segment_%020d.ts";
}

inline std::string redactedHlsUrl(const std::string& playlistUrl) {
  const auto schemeEnd = playlistUrl.find("://");
  if (schemeEnd == std::string::npos) return "<hls-playlist>";
  const auto pathStart = playlistUrl.find('/', schemeEnd + 3);
  if (pathStart == std::string::npos) return "<hls-playlist>";
  return playlistUrl.substr(0, pathStart) + "/<hls-playlist>";
}

}  // namespace corevideo::modules
