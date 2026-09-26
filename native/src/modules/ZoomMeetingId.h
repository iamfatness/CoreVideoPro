#pragma once

#include <string>
#include <string_view>

namespace corevideo::modules {

// Read only the /j/<id> path segment. A pwd query and even the Zoom host can
// contain digits; neither is part of the meeting number sent to the SDK.
inline std::string meetingIdFromJoinInput(std::string_view meetingNumber,
                                          std::string_view meetingUrl) {
  if (!meetingNumber.empty()) {
    std::string id;
    for (const char ch : meetingNumber) {
      if (ch >= '0' && ch <= '9') id.push_back(ch);
      else if (ch != ' ' && ch != '-') return {};
    }
    return id;
  }

  const auto pathEnd = meetingUrl.find_first_of("?#");
  const auto path = meetingUrl.substr(0, pathEnd);
  const auto marker = path.find("/j/");
  if (marker == std::string_view::npos) return {};
  const auto start = marker + 3;
  auto end = start;
  while (end < path.size() && path[end] >= '0' && path[end] <= '9') ++end;
  if (end == start || (end < path.size() && path[end] != '/')) return {};
  return std::string(path.substr(start, end - start));
}

// The Meeting SDK accepts the pwd token from a Zoom join link as its psw
// parameter. Preserve an explicitly supplied passcode over the link value.
inline std::string passcodeFromJoinUrl(std::string_view meetingUrl) {
  const auto queryStart = meetingUrl.find('?');
  if (queryStart == std::string_view::npos) return {};
  auto cursor = queryStart + 1;
  while (cursor < meetingUrl.size()) {
    const auto end = meetingUrl.find_first_of("&#", cursor);
    const auto field = meetingUrl.substr(cursor, end == std::string_view::npos
        ? end : end - cursor);
    if (field.substr(0, 4) == "pwd=" && field.size() > 4) {
      return std::string(field.substr(4));
    }
    if (end == std::string_view::npos || meetingUrl[end] == '#') break;
    cursor = end + 1;
  }
  return {};
}

}  // namespace corevideo::modules
