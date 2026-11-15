// corevideo-vcam-feed — the producer side of the macOS virtual camera's local
// end-to-end check (roadmap M9.2, Task 7; scripts/macos-vcam-e2e.sh).
//
//   corevideo-vcam-feed --seconds N --pattern red [--fps 60]
//
// Feeds 1920x1080 BT.601 video-range NV12 (red: Y 82, Cb 90, Cr 240) at the
// given rate through the PRODUCTION path — SinkVirtualCameraPublisher over the
// real CoreMediaIO sink (createCmioVirtualCameraSink) — so the e2e check
// exercises exactly the code the core ships, minus the compositor.
//
// The extension admits only signed producers (spec §4 law 6). Sign this binary
// with identifier us.iamfatness.corevideopro.vcam-probe and the Developer ID
// identity before running it; the e2e script does that.
//
// Reconnect: a failed connect or a lost sink is retried every 1 s, the cadence
// of the core's AsyncVirtualCameraPublisher worker, so e2e step 5 (extension
// restart) sees the same recovery the app does.
//
// Output (stdout): one `[feed] at=<unix ms> t=<s> status=<state> warning=<w>`
// line per status change, then one final JSON line:
//   {"status":"live","framesPublished":N,"framesOffered":N,"reconnects":N,
//    "warning":"","seconds":N}
// [virtualcam] lines from the publisher/sink go to the process log (stderr).
// Exit 0 when the final status is live and at least one frame was published.

#include "modules/CmioVirtualCameraSink.h"
#include "modules/SinkVirtualCameraPublisher.h"
#include "modules/VirtualCameraFrame.h"

#import <Foundation/Foundation.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <vector>

using corevideo::modules::SinkVirtualCameraPublisher;

namespace {

constexpr int kWidth = 1920;
constexpr int kHeight = 1080;

[[noreturn]] void usage() {
  std::fprintf(stderr, "usage: corevideo-vcam-feed --seconds N --pattern red [--fps 30|60]\n");
  std::exit(2);
}

long long unixMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

std::string jsonEscape(const std::string& in) {
  std::string out;
  for (char c : in) {
    if (c == '"' || c == '\\') out.push_back('\\');
    if (static_cast<unsigned char>(c) < 0x20) continue;
    out.push_back(c);
  }
  return out;
}

// BT.601 video range, the integer conversion convertBgraToNv12 uses for pure
// red (R 255, G 0, B 0): Y 82, Cb 90, Cr 240.
std::shared_ptr<const std::vector<uint8_t>> redFrame() {
  auto bytes = std::make_shared<std::vector<uint8_t>>(corevideo::modules::nv12FrameSize(kWidth, kHeight));
  const size_t luma = static_cast<size_t>(kWidth) * kHeight;
  std::memset(bytes->data(), 82, luma);
  for (size_t i = luma; i + 1 < bytes->size(); i += 2) {
    (*bytes)[i] = 90;
    (*bytes)[i + 1] = 240;
  }
  return bytes;
}

}  // namespace

int main(int argc, char** argv) {
  double seconds = 0;
  int fps = 60;
  std::string pattern;
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "--seconds" && i + 1 < argc) {
      seconds = std::atof(argv[++i]);
    } else if (arg == "--pattern" && i + 1 < argc) {
      pattern = argv[++i];
    } else if (arg == "--fps" && i + 1 < argc) {
      fps = std::atoi(argv[++i]);
    } else {
      usage();
    }
  }
  if (seconds <= 0 || pattern != "red" || (fps != 30 && fps != 60)) usage();

  @autoreleasepool {
    SinkVirtualCameraPublisher camera(corevideo::modules::createCmioVirtualCameraSink(),
                                      corevideo::modules::machHostClockNs);
    const auto frame = redFrame();
    const auto start = std::chrono::steady_clock::now();
    const auto end = start + std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                                 std::chrono::duration<double>(seconds));
    const auto period = std::chrono::nanoseconds(1'000'000'000LL / fps);
    const auto retryEvery = std::chrono::seconds(1);

    std::string lastState;
    std::string lastWarning;
    auto report = [&](bool force) {
      const auto status = camera.status();
      if (!force && status.state == lastState && status.warning == lastWarning) return;
      lastState = status.state;
      lastWarning = status.warning;
      const double t = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
      std::printf("[feed] at=%lld t=%.3f status=%s warning=%s\n", unixMs(), t, status.state.c_str(),
                  status.warning.c_str());
      std::fflush(stdout);
    };

    long long reconnects = 0;
    long long offered = 0;
    int64_t sequence = 0;
    bool everConnected = false;
    auto nextRetry = start;
    auto next = start;  // fixed anchor: overshoot never accumulates
    while (std::chrono::steady_clock::now() < end) {
      const auto now = std::chrono::steady_clock::now();
      if (camera.status().state != "live" && now >= nextRetry) {
        camera.stop();
        if (camera.start(kWidth, kHeight, fps)) {
          if (everConnected) ++reconnects;
          everConnected = true;
        }
        nextRetry = now + retryEvery;
        report(false);
      }
      if (camera.status().state == "live") {
        camera.publishNv12IdentifiedOnWorker(frame, kWidth, kHeight, ++sequence, 0);
        ++offered;
        report(false);
      }
      next += period;
      if (next < now) next = now;  // bounded catch-up after a stall
      std::this_thread::sleep_until(next);
    }

    const auto status = camera.status();
    report(true);
    std::printf("{\"status\":\"%s\",\"framesPublished\":%llu,\"framesOffered\":%lld,\"reconnects\":%lld,"
                "\"warning\":\"%s\",\"seconds\":%.1f}\n",
                status.state.c_str(), static_cast<unsigned long long>(status.framesPublished), offered,
                reconnects, jsonEscape(status.warning).c_str(), seconds);
    std::fflush(stdout);
    const bool ok = status.state == "live" && status.framesPublished > 0;
    camera.stop();
    return ok ? 0 : 1;
  }
}
