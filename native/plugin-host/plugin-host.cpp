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

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
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

// ---------------------------------------------------------------------------
// P2a probe: load the plugin module IN THIS PROCESS (never the media core) and
// interrogate its class factory. No VST3 SDK: the factory ABI is COM-style, so
// minimal C vtable declarations suffice for counting audio-effect classes. A
// plugin that crashes on load kills THIS process (nonzero exit = probe fail) —
// that is the crash-isolation posture working, not an error to prevent.
// ---------------------------------------------------------------------------
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace {

// VST3 factory ABI (COM-compatible on Windows x64).
struct PFactoryInfo {
  char vendor[64];
  char url[256];
  char email[128];
  int32_t flags;
};

struct PClassInfo {
  char cid[16];
  int32_t cardinality;
  char category[32];
  char name[64];
};

struct IPluginFactoryVtbl {
  int32_t(__stdcall* queryInterface)(void* self, const char* iid, void** obj);
  uint32_t(__stdcall* addRef)(void* self);
  uint32_t(__stdcall* release)(void* self);
  int32_t(__stdcall* getFactoryInfo)(void* self, PFactoryInfo* info);
  int32_t(__stdcall* countClasses)(void* self);
  int32_t(__stdcall* getClassInfo)(void* self, int32_t index, PClassInfo* info);
};

struct IPluginFactory {
  IPluginFactoryVtbl* vtbl;
};

using InitDllFn = bool(__stdcall*)();
using GetPluginFactoryFn = IPluginFactory*(__stdcall*)();

fs::path resolveModulePath(const fs::path& bundle) {
  std::error_code ec;
  if (!fs::is_directory(bundle, ec)) {
    return bundle;  // single-file .vst3 module
  }
  const fs::path arch = bundle / "Contents" / "x86_64-win";
  if (fs::exists(arch, ec)) {
    for (const auto& entry : fs::directory_iterator(arch, ec)) {
      if (entry.path().extension() == ".vst3") {
        return entry.path();
      }
    }
  }
  // Non-standard layouts: any .vst3 FILE anywhere inside the bundle dir.
  for (fs::recursive_directory_iterator it(bundle, fs::directory_options::skip_permission_denied, ec), end;
       it != end && !ec; it.increment(ec)) {
    if (!it->is_directory(ec) && it->path().extension() == ".vst3") {
      return it->path();
    }
  }
  return {};
}

int probeBundle(const std::string& bundlePath) {
  std::vector<std::string> reasons;
  const fs::path module = resolveModulePath(fs::path(bundlePath));
  if (module.empty()) {
    std::fprintf(stdout,
                 "{\"cmd\":\"probe-result\",\"id\":\"%s\",\"pass\":false,\"reasons\":[\"no x86_64-win module in bundle\"]}\n",
                 jsonEscape(bundlePath).c_str());
    return 0;
  }

  HMODULE library = ::LoadLibraryExA(module.string().c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
  if (library == nullptr) {
    std::fprintf(stdout,
                 "{\"cmd\":\"probe-result\",\"id\":\"%s\",\"pass\":false,\"reasons\":[\"module failed to load (code %lu)\"]}\n",
                 jsonEscape(bundlePath).c_str(), ::GetLastError());
    return 0;
  }

  const auto initDll = reinterpret_cast<InitDllFn>(::GetProcAddress(library, "InitDll"));
  const auto getFactory = reinterpret_cast<GetPluginFactoryFn>(::GetProcAddress(library, "GetPluginFactory"));
  if (getFactory == nullptr) {
    std::fprintf(stdout,
                 "{\"cmd\":\"probe-result\",\"id\":\"%s\",\"pass\":false,\"reasons\":[\"no GetPluginFactory export - not a VST3 module\"]}\n",
                 jsonEscape(bundlePath).c_str());
    return 0;
  }
  if (initDll != nullptr && !initDll()) {
    std::fprintf(stdout,
                 "{\"cmd\":\"probe-result\",\"id\":\"%s\",\"pass\":false,\"reasons\":[\"InitDll returned false\"]}\n",
                 jsonEscape(bundlePath).c_str());
    return 0;
  }

  IPluginFactory* factory = getFactory();
  if (factory == nullptr || factory->vtbl == nullptr) {
    std::fprintf(stdout,
                 "{\"cmd\":\"probe-result\",\"id\":\"%s\",\"pass\":false,\"reasons\":[\"GetPluginFactory returned null\"]}\n",
                 jsonEscape(bundlePath).c_str());
    return 0;
  }

  PFactoryInfo factoryInfo{};
  factory->vtbl->getFactoryInfo(factory, &factoryInfo);
  const int32_t classCount = factory->vtbl->countClasses(factory);
  int audioClasses = 0;
  std::string firstAudioClassName;
  for (int32_t index = 0; index < classCount; ++index) {
    PClassInfo classInfo{};
    if (factory->vtbl->getClassInfo(factory, index, &classInfo) != 0) {
      continue;
    }
    const std::string category(classInfo.category, strnlen(classInfo.category, sizeof(classInfo.category)));
    const std::string className(classInfo.name, strnlen(classInfo.name, sizeof(classInfo.name)));
    // One line per class — the core ignores these; they make probe verdicts
    // debuggable from the raw output.
    std::fprintf(stdout, "{\"cmd\":\"probe-class\",\"index\":%d,\"category\":\"%s\",\"name\":\"%s\"}\n",
                 index, jsonEscape(category).c_str(), jsonEscape(className).c_str());
    if (category == "Audio Module Class") {
      ++audioClasses;
      if (firstAudioClassName.empty()) {
        firstAudioClassName = className;
      }
    }
  }
  factory->vtbl->release(factory);

  const bool pass = audioClasses > 0;
  const char* failReason = classCount == 0
                               ? "\"factory reports no classes (vendor shell plugins need their own scanner)\""
                               : "\"module exposes no Audio Module Class\"";
  std::fprintf(stdout,
               "{\"cmd\":\"probe-result\",\"id\":\"%s\",\"pass\":%s,\"vendor\":\"%s\",\"audioClasses\":%d,\"className\":\"%s\",\"reasons\":[%s]}\n",
               jsonEscape(bundlePath).c_str(), pass ? "true" : "false",
               jsonEscape(std::string(factoryInfo.vendor, strnlen(factoryInfo.vendor, sizeof(factoryInfo.vendor)))).c_str(),
               audioClasses, jsonEscape(firstAudioClassName).c_str(),
               pass ? "" : failReason);
  std::fflush(stdout);
  // Deliberately no FreeLibrary/ExitDll: some plugins crash on unload, and this
  // process is about to exit anyway — the OS reclaims everything.
  return 0;
}

}  // namespace
#endif  // _WIN32

int main(int argc, char** argv) {
  const std::string mode = argc > 1 ? argv[1] : "";
  if (mode == "--probe" && argc > 2) {
#ifdef _WIN32
    return probeBundle(argv[2]);
#else
    std::fprintf(stdout, "{\"cmd\":\"probe-result\",\"id\":\"%s\",\"pass\":false,\"reasons\":[\"probe unsupported on this platform\"]}\n",
                 jsonEscape(argv[2]).c_str());
    return 0;
#endif
  }

  if (mode != "--scan") {
    std::fprintf(stdout, "{\"cmd\":\"error\",\"msg\":\"corevideo-plugin-host supports --scan and --probe <bundle>\"}\n");
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
