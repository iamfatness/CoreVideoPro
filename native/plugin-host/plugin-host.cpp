// corevideo-plugin-host — out-of-process VST3 host (docs/vst-host-spec.md).
//
// Modes (all one-shot except --serve):
//   --scan                       enumerate VST3 bundles (P1, filesystem only)
//   --probe <bundle>             load the module, list classes, pass/fail (P2a)
//   --process <bundle> <class>   instantiate + push 1s of 440Hz through the
//                                plugin, print a process-result JSON line (P2c
//                                terminal proof — no app required)
//   --serve <instance>           resident SHM audio-exchange loop (P2b/P2c)
//
// NO VST3 SDK dependency (GPLv3 — house rule): the COM ABI is declared raw in
// vst-abi.h and validated by static_asserts + live-plugin runs. Loading
// plugin code happens in THIS process, never the media core — a crashing
// plugin kills this process (nonzero exit / host death), which the core
// treats as an honest verdict (probe fail / bypass to built-ins).
//
// Output protocol (stdout, one JSON object per line):
//   {"cmd":"plugin","id":"<path>","name":"...","vendor":"...","probe":"pending"}
//   {"cmd":"scan-complete","count":N}
//   {"cmd":"probe-class"...} / {"cmd":"probe-result"...}
//   {"cmd":"process-result","pass":bool,"rmsInDb":x,"rmsOutDb":x,"changed":bool,"error":"..."}
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
// P2a probe + P2c instantiate/process: load the plugin module IN THIS PROCESS
// (never the media core) via the raw COM ABI in vst-abi.h. No VST3 SDK. A
// plugin that crashes on load kills THIS process (nonzero exit = probe fail /
// host death = bypass) — that is the crash-isolation posture working, not an
// error to prevent.
// ---------------------------------------------------------------------------
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <objbase.h>

#include <cmath>
#include <map>
#include <memory>

#include "vst-abi.h"
#include "vst-processor.h"

namespace {

using corevideo::vst3::GetPluginFactoryFn;
using corevideo::vst3::InitDllFn;
using corevideo::vst3::IPluginFactory;
using corevideo::vst3::PClassInfo;
using corevideo::vst3::PFactoryInfo;

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

// Loads a .vst3 bundle's module and returns its class factory. Deliberately
// NO FreeLibrary/ExitDll anywhere in this executable: some plugins crash on
// unload, and every mode either exits (OS reclaims) or keeps plugins resident
// for its lifetime.
struct LoadedModule {
  IPluginFactory* factory = nullptr;
  std::string error;
};

LoadedModule loadPluginModule(const std::string& bundlePath) {
  LoadedModule loaded;
  const fs::path module = resolveModulePath(fs::path(bundlePath));
  if (module.empty()) {
    loaded.error = "no x86_64-win module in bundle";
    return loaded;
  }
  HMODULE library = ::LoadLibraryExA(module.string().c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
  if (library == nullptr) {
    loaded.error = "module failed to load (code " + std::to_string(::GetLastError()) + ")";
    return loaded;
  }
  const auto initDll = reinterpret_cast<InitDllFn>(::GetProcAddress(library, "InitDll"));
  const auto getFactory = reinterpret_cast<GetPluginFactoryFn>(::GetProcAddress(library, "GetPluginFactory"));
  if (getFactory == nullptr) {
    loaded.error = "no GetPluginFactory export - not a VST3 module";
    return loaded;
  }
  if (initDll != nullptr && !initDll()) {
    loaded.error = "InitDll returned false";
    return loaded;
  }
  IPluginFactory* factory = getFactory();
  if (factory == nullptr || factory->vtbl == nullptr) {
    loaded.error = "GetPluginFactory returned null";
    return loaded;
  }
  loaded.factory = factory;
  return loaded;
}

int probeBundle(const std::string& bundlePath) {
  const LoadedModule loaded = loadPluginModule(bundlePath);
  if (loaded.factory == nullptr) {
    std::fprintf(stdout, "{\"cmd\":\"probe-result\",\"id\":\"%s\",\"pass\":false,\"reasons\":[\"%s\"]}\n",
                 jsonEscape(bundlePath).c_str(), jsonEscape(loaded.error).c_str());
    return 0;
  }
  IPluginFactory* factory = loaded.factory;

  PFactoryInfo factoryInfo{};
  factory->vtbl->getFactoryInfo(factory, &factoryInfo);
  const int32_t classCount = factory->vtbl->countClasses(factory);
  std::vector<std::string> audioClassNames;
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
      audioClassNames.push_back(className);
    }
  }
  factory->vtbl->release(factory);

  const bool pass = !audioClassNames.empty();
  const char* failReason = classCount == 0
                               ? "\"factory reports no classes (vendor shell plugins need their own scanner)\""
                               : "\"module exposes no Audio Module Class\"";
  // P2c: classNames carries EVERY audio class so shell bundles (Waves) can be
  // selected per class from the core's insert names ("vst:<class>").
  std::string classNamesJson;
  for (const auto& className : audioClassNames) {
    if (!classNamesJson.empty()) {
      classNamesJson += ",";
    }
    classNamesJson += "\"" + jsonEscape(className) + "\"";
  }
  std::fprintf(stdout,
               "{\"cmd\":\"probe-result\",\"id\":\"%s\",\"pass\":%s,\"vendor\":\"%s\",\"audioClasses\":%zu,"
               "\"className\":\"%s\",\"classNames\":[%s],\"reasons\":[%s]}\n",
               jsonEscape(bundlePath).c_str(), pass ? "true" : "false",
               jsonEscape(std::string(factoryInfo.vendor, strnlen(factoryInfo.vendor, sizeof(factoryInfo.vendor)))).c_str(),
               audioClassNames.size(),
               jsonEscape(pass ? audioClassNames.front() : std::string{}).c_str(),
               classNamesJson.c_str(), pass ? "" : failReason);
  std::fflush(stdout);
  // Deliberately no FreeLibrary/ExitDll (see loadPluginModule).
  return 0;
}

// ---------------------------------------------------------------------------
// P2c CLI proof: --process <bundle> <className>. Instantiates the plugin and
// pushes ~1s of 440Hz stereo sine through per-block process calls, then prints
// a single machine-readable verdict. A crash mid-run = process death = no
// process-result line, which IS the verdict for the caller.
// ---------------------------------------------------------------------------
void printProcessResult(bool pass, double rmsInDb, double rmsOutDb, bool changed,
                        const std::string& plugin, uint32_t latencySamples, const std::string& error) {
  std::fprintf(stdout,
               "{\"cmd\":\"process-result\",\"pass\":%s,\"plugin\":\"%s\",\"rmsInDb\":%.2f,"
               "\"rmsOutDb\":%.2f,\"changed\":%s,\"latencySamples\":%u,\"error\":\"%s\"}\n",
               pass ? "true" : "false", jsonEscape(plugin).c_str(), rmsInDb, rmsOutDb,
               changed ? "true" : "false", latencySamples, jsonEscape(error).c_str());
  std::fflush(stdout);
}

int processBundle(const std::string& bundlePath, const std::string& className) {
  const LoadedModule loaded = loadPluginModule(bundlePath);
  if (loaded.factory == nullptr) {
    printProcessResult(false, 0.0, 0.0, false, className, 0, loaded.error);
    return 0;
  }

  corevideo::vsthost::VstPluginInstance instance;
  corevideo::vsthost::VstProcessorConfig config;
  config.sampleRate = 48000.0;
  config.maxBlockFrames = 4096;
  if (!instance.start(loaded.factory, className, config)) {
    printProcessResult(false, 0.0, 0.0, false, className, 0, instance.lastError());
    return 0;
  }

  // ~1 second of 440 Hz stereo at 48 kHz, in realistic 960-frame (20ms) ticks.
  constexpr int kFramesPerBlock = 960;
  constexpr int kBlocks = 50;
  constexpr double kAmplitude = 0.5;
  constexpr double kTwoPi = 6.283185307179586;
  std::vector<float> block(kFramesPerBlock * 2);
  std::vector<float> original(kFramesPerBlock * 2);
  double sumSquaresIn = 0.0;
  double sumSquaresOut = 0.0;
  bool changed = false;
  std::string error;
  bool pass = true;
  int64_t sampleIndex = 0;
  for (int blockIndex = 0; blockIndex < kBlocks && pass; ++blockIndex) {
    for (int frame = 0; frame < kFramesPerBlock; ++frame) {
      const auto value = static_cast<float>(
          kAmplitude * std::sin(kTwoPi * 440.0 * static_cast<double>(sampleIndex + frame) / 48000.0));
      block[frame * 2] = value;
      block[frame * 2 + 1] = value;
    }
    sampleIndex += kFramesPerBlock;
    original = block;
    for (const float sample : block) {
      sumSquaresIn += static_cast<double>(sample) * sample;
    }
    if (!instance.processInterleavedStereo(block.data(), static_cast<int32_t>(block.size()))) {
      error = instance.lastError();
      pass = false;
      break;
    }
    for (size_t index = 0; index < block.size(); ++index) {
      sumSquaresOut += static_cast<double>(block[index]) * block[index];
      if (std::fabs(block[index] - original[index]) > 1e-6f) {
        changed = true;
      }
    }
  }

  const auto toDb = [](double sumSquares, int64_t count) {
    if (count <= 0) {
      return -120.0;
    }
    const double rms = std::sqrt(sumSquares / static_cast<double>(count));
    return rms > 1e-6 ? 20.0 * std::log10(rms) : -120.0;
  };
  const int64_t totalSamples = static_cast<int64_t>(kFramesPerBlock) * 2 * kBlocks;
  printProcessResult(pass, toDb(sumSquaresIn, totalSamples),
                     pass ? toDb(sumSquaresOut, totalSamples) : -120.0, changed,
                     instance.activeClassName().empty() ? className : instance.activeClassName(),
                     instance.latencySamples(), error);
  instance.shutdown();
  return 0;
}

}  // namespace
#endif  // _WIN32

// ---------------------------------------------------------------------------
// P2b/P2c serve: the resident audio-exchange loop. The core creates the SHM
// block and both events, then spawns us with the instance name; we open them,
// wait for req, process the block IN PLACE, publish seqOut, signal done.
//
// P2c: the block carries a plugin selection. Empty = the legacy -6dB test
// processor (the plain "vst" insert-name rig drill). A selection loads the
// real plugin ON THIS THREAD (the core deadline-bypasses during the load —
// misses are honest, not a hang) and caches it for the process lifetime, so
// alternating selections across buses never reload. Load/process failures
// leave the block UNTOUCHED (bypass) and surface through the status fields +
// a LOUD serve-log error line.
// Exits after 30s with no requests (parent gone) — the core respawns on demand.
// ---------------------------------------------------------------------------
#include "host-transport.h"

#ifdef _WIN32
namespace {

void applyTestGain(corevideo::pluginhost::HostAudioBlock* block) {
  const int32_t count = block->sampleCount < 0 ? 0
                        : (block->sampleCount > corevideo::pluginhost::kHostBlockMaxSamples
                               ? corevideo::pluginhost::kHostBlockMaxSamples
                               : block->sampleCount);
  for (int32_t index = 0; index < count; ++index) {
    block->pcm[index] *= 0.5012f;  // -6 dB test processor (legacy "vst" insert name)
  }
}

// One loaded plugin per distinct (bundle, class) selection, kept for the
// process lifetime (unload crashes are a known plugin bug class).
struct ServeSlot {
  corevideo::vsthost::VstPluginInstance instance;
  std::string error;  // load-time failure, sticky
};

int serveInstance(const std::string& instance) {
  using namespace corevideo::pluginhost;
  HANDLE shm = ::OpenFileMappingA(FILE_MAP_ALL_ACCESS, FALSE, hostShmName(instance).c_str());
  HANDLE req = ::OpenEventA(SYNCHRONIZE, FALSE, hostReqEventName(instance).c_str());
  HANDLE done = ::OpenEventA(EVENT_MODIFY_STATE, FALSE, hostDoneEventName(instance).c_str());
  if (shm == nullptr || req == nullptr || done == nullptr) {
    std::fprintf(stdout, "{\"cmd\":\"error\",\"msg\":\"serve: transport objects missing for '%s'\"}\n",
                 jsonEscape(instance).c_str());
    return 3;
  }
  auto* block = static_cast<HostAudioBlock*>(::MapViewOfFile(shm, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(HostAudioBlock)));
  if (block == nullptr || block->magic != kHostBlockMagic) {
    std::fprintf(stdout, "{\"cmd\":\"error\",\"msg\":\"serve: block unmapped or bad magic (host/core version mismatch?)\"}\n");
    return 3;
  }

  std::fprintf(stdout, "{\"cmd\":\"serving\",\"instance\":\"%s\"}\n", jsonEscape(instance).c_str());
  std::fflush(stdout);

  std::map<std::string, std::unique_ptr<ServeSlot>> slots;
  std::string publishedStatusKey;  // dedupes statusGeneration bumps

  const auto publishStatus = [&](int32_t code, const std::string& activePlugin, const std::string& error) {
    const std::string key = std::to_string(code) + "\x1f" + activePlugin + "\x1f" + error;
    if (key == publishedStatusKey) {
      return;
    }
    publishedStatusKey = key;
    block->statusCode = code;
    copyBlockString(block->activePlugin, activePlugin.c_str());
    copyBlockString(block->lastError, error.c_str());
    block->statusGeneration = block->statusGeneration + 1;
  };

  for (;;) {
    const DWORD wait = ::WaitForSingleObject(req, 30000);
    if (wait != WAIT_OBJECT_0) {
      return 0;  // idle 30s — parent gone or bypassed; exit, the core respawns
    }
    // Capture the sequence BEFORE processing: if the core abandons this tick
    // and writes a new one while we work, seqOut must NOT accidentally match
    // the new seqIn (the stale-completion discard depends on it).
    const uint32_t seq = block->seqIn;

    const std::string bundle(block->pluginBundle,
                             strnlen(block->pluginBundle, sizeof(block->pluginBundle)));
    const std::string className(block->pluginClass,
                                strnlen(block->pluginClass, sizeof(block->pluginClass)));
    if (bundle.empty()) {
      applyTestGain(block);
      publishStatus(kHostStatusTestProcessor, "test:-6dB", "");
    } else {
      const std::string slotKey = bundle + "\x1f" + className;
      auto found = slots.find(slotKey);
      if (found == slots.end()) {
        // First request for this selection: load NOW, on this thread. The
        // core keeps bypassing on its 4ms deadline while we do — honest
        // misses, never a stall on its audio worker.
        auto slot = std::make_unique<ServeSlot>();
        const LoadedModule loaded = loadPluginModule(bundle);
        if (loaded.factory == nullptr) {
          slot->error = loaded.error;
        } else {
          corevideo::vsthost::VstProcessorConfig config;
          config.sampleRate = 48000.0;
          config.maxBlockFrames = kHostBlockMaxSamples / 2;
          if (!slot->instance.start(loaded.factory, className, config)) {
            slot->error = slot->instance.lastError();
          }
        }
        if (!slot->error.empty()) {
          std::fprintf(stdout, "{\"cmd\":\"error\",\"msg\":\"serve: load '%s' / '%s' FAILED: %s\"}\n",
                       jsonEscape(bundle).c_str(), jsonEscape(className).c_str(),
                       jsonEscape(slot->error).c_str());
        } else {
          std::fprintf(stdout, "{\"cmd\":\"loaded\",\"bundle\":\"%s\",\"className\":\"%s\",\"latencySamples\":%u}\n",
                       jsonEscape(bundle).c_str(),
                       jsonEscape(slot->instance.activeClassName()).c_str(),
                       slot->instance.latencySamples());
        }
        std::fflush(stdout);
        found = slots.emplace(slotKey, std::move(slot)).first;
      }

      ServeSlot& slot = *found->second;
      if (!slot.error.empty()) {
        // Bypass: block left untouched. The core hears its own audio and the
        // snapshot shows the error — never a fake "processing" status.
        publishStatus(kHostStatusPluginFailed, className, slot.error);
      } else if (block->sampleCount < 2 || block->sampleCount > kHostBlockMaxSamples) {
        // Transient/garbage block header: skip (untouched), don't poison the slot.
      } else if (slot.instance.processInterleavedStereo(block->pcm, block->sampleCount)) {
        publishStatus(kHostStatusPluginActive, slot.instance.activeClassName(), "");
      } else {
        slot.error = slot.instance.lastError().empty() ? "process() failed" : slot.instance.lastError();
        std::fprintf(stdout, "{\"cmd\":\"error\",\"msg\":\"serve: process '%s' FAILED: %s\"}\n",
                     jsonEscape(className).c_str(), jsonEscape(slot.error).c_str());
        std::fflush(stdout);
        publishStatus(kHostStatusPluginFailed, className, slot.error);
      }
    }

    block->seqOut = seq;
    ::SetEvent(done);
  }
}

}  // namespace
#endif  // _WIN32

int main(int argc, char** argv) {
  const std::string mode = argc > 1 ? argv[1] : "";
#ifdef _WIN32
  if (mode == "--serve" || mode == "--probe" || mode == "--process") {
    // Plugins routinely assume an STA COM apartment on their loading thread.
    ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  }
#endif

  if (mode == "--serve" && argc > 2) {
#ifdef _WIN32
    return serveInstance(argv[2]);
#else
    std::fprintf(stdout, "{\"cmd\":\"error\",\"msg\":\"serve unsupported on this platform\"}\n");
    return 3;
#endif
  }

  if (mode == "--probe" && argc > 2) {
#ifdef _WIN32
    return probeBundle(argv[2]);
#else
    std::fprintf(stdout, "{\"cmd\":\"probe-result\",\"id\":\"%s\",\"pass\":false,\"reasons\":[\"probe unsupported on this platform\"]}\n",
                 jsonEscape(argv[2]).c_str());
    return 0;
#endif
  }

  if (mode == "--process" && argc > 2) {
#ifdef _WIN32
    return processBundle(argv[2], argc > 3 ? argv[3] : "");
#else
    std::fprintf(stdout, "{\"cmd\":\"process-result\",\"pass\":false,\"changed\":false,\"error\":\"process unsupported on this platform\"}\n");
    return 0;
#endif
  }

  if (mode != "--scan") {
    std::fprintf(stdout,
                 "{\"cmd\":\"error\",\"msg\":\"corevideo-plugin-host supports --scan, --probe <bundle>, "
                 "--process <bundle> <className>, --serve <instance>\"}\n");
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
