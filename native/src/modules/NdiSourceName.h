#ifndef COREVIDEO_MODULES_NDI_SOURCE_NAME_H
#define COREVIDEO_MODULES_NDI_SOURCE_NAME_H

#include <algorithm>
#include <cctype>
#include <string>
#include <vector>

namespace corevideo::modules {

// Pure, stub-testable mirror of `src/engine/ndiOutput.ts::parseNdiSourceName`.
// The native NDI sender must honor exactly the same source-name rules the
// renderer validates before it ships an `ndi` destination, so the runtime
// registers an mDNS source name the operator already saw accepted in the UI.
//
// Source names follow the convention "MACHINE NAME (Program Name)". We accept
// either the full canonical form or just the program-name portion, normalizing
// to canonical form ("MACHINE (Program)") on success. The machine portion is
// upper-cased; a bare program name uses the "COREVIDEO" placeholder machine.
struct NdiSourceName {
  bool valid = false;
  std::string machineName;
  std::string programName;
  std::string canonical;
  std::vector<std::string> errors;
  std::vector<std::string> warnings;
};

inline constexpr int kNdiMaxSourceNameLength = 64;

namespace detail {

inline std::string trimAscii(const std::string& value) {
  size_t begin = 0;
  size_t end = value.size();
  while (begin < end && std::isspace(static_cast<unsigned char>(value[begin]))) {
    ++begin;
  }
  while (end > begin && std::isspace(static_cast<unsigned char>(value[end - 1]))) {
    --end;
  }
  return value.substr(begin, end - begin);
}

inline std::string upperAscii(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
    return static_cast<char>(std::toupper(ch));
  });
  return value;
}

inline std::string lowerAscii(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
    return static_cast<char>(std::tolower(ch));
  });
  return value;
}

// Matches `/[<>:"\/\\|?*\x00-\x1f]/` from ndiOutput.ts (NDI reserved chars).
inline bool hasReservedChar(const std::string& value) {
  for (const char raw : value) {
    const unsigned char ch = static_cast<unsigned char>(raw);
    if (ch <= 0x1f) {
      return true;
    }
    switch (raw) {
      case '<':
      case '>':
      case ':':
      case '"':
      case '/':
      case '\\':
      case '|':
      case '?':
      case '*':
        return true;
      default:
        break;
    }
  }
  return false;
}

}  // namespace detail

inline NdiSourceName parseNdiSourceName(const std::string& input) {
  NdiSourceName result;
  const std::string trimmed = detail::trimAscii(input);

  if (trimmed.empty()) {
    result.errors.push_back("NDI source name is required.");
    return result;
  }

  if (trimmed.size() > static_cast<size_t>(kNdiMaxSourceNameLength)) {
    result.errors.push_back("NDI source name must be 64 characters or fewer.");
  }

  if (detail::hasReservedChar(trimmed)) {
    result.errors.push_back("NDI source name contains invalid characters.");
  }

  std::string machineName;
  std::string programName;

  // Detect canonical "MACHINE (Program)" form. We look for a trailing
  // "( ... )" group: everything before it is the machine, the parenthesized
  // body is the program.
  const auto open = trimmed.find('(');
  if (open != std::string::npos && !trimmed.empty() && trimmed.back() == ')' && open + 1 < trimmed.size()) {
    machineName = detail::upperAscii(detail::trimAscii(trimmed.substr(0, open)));
    programName = detail::trimAscii(trimmed.substr(open + 1, trimmed.size() - open - 2));
  } else {
    machineName = "COREVIDEO";
    programName = trimmed;
    result.warnings.push_back("NDI source will be published as \"COREVIDEO (" + trimmed + ")\".");
  }

  if (programName.empty()) {
    result.errors.push_back("NDI program name cannot be empty.");
  }

  const std::string loweredProgram = detail::lowerAscii(programName);
  if (loweredProgram == "program" || loweredProgram == "preview") {
    // Conventional names — fine.
  } else if (programName.size() < 2) {
    result.warnings.push_back("Short NDI program names may conflict with other sources on the network.");
  }

  if (!result.errors.empty()) {
    return result;
  }

  result.valid = true;
  result.machineName = machineName;
  result.programName = programName;
  result.canonical = machineName + " (" + programName + ")";
  return result;
}

}  // namespace corevideo::modules

#endif  // COREVIDEO_MODULES_NDI_SOURCE_NAME_H
