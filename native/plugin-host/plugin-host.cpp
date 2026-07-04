// corevideo-plugin-host — out-of-process VST3 host (docs/vst-host-spec.md).
//
// P1 scope: `--scan` one-shot mode. Enumerates the system VST3 directories and
// prints one JSON line per plugin bundle found, then a scan-complete line, and
// exits. NO plugin code is loaded (probe/load are later phases) and no VST3 SDK
// is required — .vst3 bundles are directories (with Contents/moduleinfo.json in
// modern bundles) or bare module files; discovery is pure filesystem metadata.
// Running discovery in THIS process (never the media core) is the safety
// posture: plugin paths are untrusted input, and even reading their metadata
// stays out of the real-time process.
//
// Output protocol (stdout, one JSON object per line):
//   {"cmd":"plugin","id":"<path>","name":"...","vendor":"...","probe":"pending"}
//   {"cmd":"scan-complete","count":N}
// Errors are {"cmd":"error","msg":"..."} and never abort remaining roots.

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

std::string jsonEscape(const std::string& value) {
  std::string escaped;
  escaped.reserve(value.size() + 8);
  for (const char character : value) {
    switch (character) {
      case '"': escaped += "\\\""; break;
      case '\\': escaped += "\\\\"; break;
      case '\n': escaped += "\\n"; break;
      case '\r': escaped += "\\r"; break;
      case '\t': escaped += "\\t"; break;
      default:
        if (static_cast<unsigned char>(character) < 0x20) {
          char buffer[8];
          std::snprintf(buffer, sizeof(buffer), "\\u%04x", character);
          escaped += buffer;
        } else {
          escaped += character;
        }
    }
  }
  return escaped;
}

// Best-effort value extraction from moduleinfo.json without a JSON dependency:
// finds `"key"` and returns the next quoted string. Good enough for the
// display-name/vendor metadata scan needs; the id stays the bundle path.
std::string extractJsonString(const std::string& document, const std::string& key) {
  const auto keyPos = document.find("\"" + key + "\"");
  if (keyPos == std::string::npos) {
    return {};
  }
  const auto colon = document.find(':', keyPos);
  if (colon == std::string::npos) {
    return {};
  }
  const auto open = document.find('"', colon);
  if (open == std::string::npos) {
    return {};
  }
  const auto close = document.find('"', open + 1);
  if (close == std::string::npos) {
    return {};
  }
  return document.substr(open + 1, close - open - 1);
}

struct ScannedPlugin {
  std::string id;      // bundle path (stable identity for load/probe later)
  std::string name;    // moduleinfo Name, else the bundle stem
  std::string vendor;  // moduleinfo Vendor, else empty
};

void describeBundle(const fs::path& bundle, std::vector<ScannedPlugin>& plugins) {
  ScannedPlugin plugin;
  plugin.id = bundle.generic_string();  // forward slashes - simplest JSON both sides
  plugin.name = bundle.stem().string();

  const fs::path moduleInfo = bundle / "Contents" / "moduleinfo.json";
  std::error_code ec;
  if (fs::exists(moduleInfo, ec)) {
    std::ifstream stream(moduleInfo, std::ios::binary);
    if (stream) {
      std::string document((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
      const auto name = extractJsonString(document, "Name");
      if (!name.empty()) {
        plugin.name = name;
      }
      plugin.vendor = extractJsonString(document, "Vendor");
    }
  }

  plugins.push_back(std::move(plugin));
}

void scanRoot(const fs::path& root, std::vector<ScannedPlugin>& plugins) {
  std::error_code ec;
  if (!fs::exists(root, ec) || !fs::is_directory(root, ec)) {
    return;
  }
  for (fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, ec), end;
       it != end && !ec; it.increment(ec)) {
    const auto& entry = *it;
    if (entry.path().extension() == ".vst3") {
      describeBundle(entry.path(), plugins);
      // A .vst3 bundle directory can contain nested module files with the same
      // extension — describe the bundle once, don't descend into it.
      if (entry.is_directory(ec)) {
        it.disable_recursion_pending();
      }
    }
  }
}

std::vector<fs::path> scanRoots() {
  std::vector<fs::path> roots;
  if (const char* commonFiles = std::getenv("COMMONPROGRAMFILES")) {
    roots.emplace_back(fs::path(commonFiles) / "VST3");
  }
  if (const char* commonFilesX86 = std::getenv("COMMONPROGRAMFILES(X86)")) {
    roots.emplace_back(fs::path(commonFilesX86) / "VST3");
  }
  if (const char* extra = std::getenv("COREVIDEO_VST3_SCAN_PATH")) {
    roots.emplace_back(fs::path(extra));  // test/dev override + additional dir
  }
  return roots;
}

}  // namespace

int main(int argc, char** argv) {
  const bool scanMode = argc > 1 && std::string(argv[1]) == "--scan";
  if (!scanMode) {
    std::fprintf(stdout, "{\"cmd\":\"error\",\"msg\":\"corevideo-plugin-host P1 supports --scan only\"}\n");
    return 2;
  }

  std::vector<ScannedPlugin> plugins;
  for (const auto& root : scanRoots()) {
    scanRoot(root, plugins);
  }

  for (const auto& plugin : plugins) {
    std::fprintf(stdout, "{\"cmd\":\"plugin\",\"id\":\"%s\",\"name\":\"%s\",\"vendor\":\"%s\",\"probe\":\"pending\"}\n",
                 jsonEscape(plugin.id).c_str(), jsonEscape(plugin.name).c_str(), jsonEscape(plugin.vendor).c_str());
  }
  std::fprintf(stdout, "{\"cmd\":\"scan-complete\",\"count\":%zu}\n", plugins.size());
  std::fflush(stdout);
  return 0;
}
