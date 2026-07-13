// Browser sources (BR-1): restart-policy state machine, SHM seqlock frame
// round-trip, add-request validation, and the MediaCore command/telemetry wiring
// (stub modules, no real host process — the adapter is pointed at a nonexistent
// exe so supervision takes the loud-failure path deterministically).

#ifdef COREVIDEO_USE_SYSTEM_GTEST
#include <gtest/gtest.h>
#else
#include "gtest/gtest.h"
#endif

#include "core/MediaCore.h"
#include "modules/BrowserHostRestartPolicy.h"
#include "modules/BrowserSourceHostAdapter.h"
#include "modules/BrowserSourceShm.h"
#include "rpc/Json.h"

#include <string>
#include <vector>

using corevideo::core::MediaCore;
using corevideo::modules::BrowserHostRestartPolicy;
using corevideo::modules::BrowserSourceHostAdapter;
namespace browsershm = corevideo::modules::browsershm;
using corevideo::rpc::Json;

// ---------------------------------------------------------------------------
// BrowserHostRestartPolicy
// ---------------------------------------------------------------------------

TEST(BrowserHostRestartPolicy, BacksOffExponentiallyAndCapsAtSixtySeconds) {
  EXPECT_EQ(BrowserHostRestartPolicy::backoffDelayMs(1), 5000);
  EXPECT_EQ(BrowserHostRestartPolicy::backoffDelayMs(2), 10000);
  EXPECT_EQ(BrowserHostRestartPolicy::backoffDelayMs(3), 20000);
  EXPECT_EQ(BrowserHostRestartPolicy::backoffDelayMs(4), 40000);
  EXPECT_EQ(BrowserHostRestartPolicy::backoffDelayMs(5), 60000);
  EXPECT_EQ(BrowserHostRestartPolicy::backoffDelayMs(9), 60000);
}

TEST(BrowserHostRestartPolicy, GivesUpAfterFiveConsecutiveFailures) {
  BrowserHostRestartPolicy policy;
  int64_t now = 0;
  EXPECT_TRUE(policy.shouldAttemptRestart(now));  // first attempt is immediate
  for (int failure = 1; failure <= 4; ++failure) {
    policy.onFailure(now);
    EXPECT_FALSE(policy.gaveUp());
    EXPECT_FALSE(policy.shouldAttemptRestart(now));  // inside the backoff window
    now = policy.nextAttemptAtMs();
    EXPECT_TRUE(policy.shouldAttemptRestart(now));  // window elapsed
  }
  policy.onFailure(now);  // fifth consecutive failure
  EXPECT_TRUE(policy.gaveUp());
  EXPECT_FALSE(policy.shouldAttemptRestart(now + 3600000));  // no churn, ever
}

TEST(BrowserHostRestartPolicy, FrameResetsTheStreakAndReloadClearsGiveUp) {
  BrowserHostRestartPolicy policy;
  for (int i = 0; i < 3; ++i) {
    policy.onFailure(1000);
  }
  EXPECT_EQ(policy.consecutiveFailures(), 3);
  policy.onHealthy();  // a real frame landed
  EXPECT_EQ(policy.consecutiveFailures(), 0);
  EXPECT_TRUE(policy.shouldAttemptRestart(1001));

  for (int i = 0; i < 5; ++i) {
    policy.onFailure(2000);
  }
  EXPECT_TRUE(policy.gaveUp());
  policy.reset();  // operator reload
  EXPECT_FALSE(policy.gaveUp());
  EXPECT_TRUE(policy.shouldAttemptRestart(2001));
}

// ---------------------------------------------------------------------------
// BrowserSourceShm seqlock round-trip (pure buffer, no Win32)
// ---------------------------------------------------------------------------

TEST(BrowserSourceShm, WriteThenReadRoundTripsPixelsAndDims) {
  const int width = 4;
  const int height = 3;
  std::vector<uint8_t> buffer(browsershm::mappingBytes(width, height), 0);
  std::vector<uint8_t> bgra(browsershm::frameBytes(width, height));
  for (std::size_t i = 0; i < bgra.size(); ++i) {
    bgra[i] = static_cast<uint8_t>(i * 7 + 3);
  }

  ASSERT_TRUE(browsershm::writeFrame(buffer.data(), buffer.size(), bgra.data(), width, height));

  uint32_t lastSequence = 0;
  const auto read = browsershm::readNewFrame(buffer.data(), buffer.size(), lastSequence);
  ASSERT_TRUE(read.gotNewFrame);
  EXPECT_EQ(read.width, width);
  EXPECT_EQ(read.height, height);
  ASSERT_TRUE(read.pixels != nullptr);
  EXPECT_TRUE(*read.pixels == bgra);

  // Nothing new: the same sequence must not be re-delivered.
  const auto again = browsershm::readNewFrame(buffer.data(), buffer.size(), lastSequence);
  EXPECT_FALSE(again.gotNewFrame);

  // A second publish advances the sequence and delivers again.
  bgra[0] = 0xFF;
  ASSERT_TRUE(browsershm::writeFrame(buffer.data(), buffer.size(), bgra.data(), width, height));
  const auto second = browsershm::readNewFrame(buffer.data(), buffer.size(), lastSequence);
  ASSERT_TRUE(second.gotNewFrame);
  EXPECT_EQ((*second.pixels)[0], 0xFF);
}

TEST(BrowserSourceShm, RejectsMidUpdateAndOversizedFrames) {
  const int width = 2;
  const int height = 2;
  std::vector<uint8_t> buffer(browsershm::mappingBytes(width, height), 0);
  std::vector<uint8_t> bgra(browsershm::frameBytes(width, height), 0xAB);
  ASSERT_TRUE(browsershm::writeFrame(buffer.data(), buffer.size(), bgra.data(), width, height));

  // Odd sequence = writer mid-update: reader must refuse.
  browsershm::storeSeq(buffer.data(), browsershm::loadSeq(buffer.data()) | 1u);
  uint32_t lastSequence = 0;
  EXPECT_FALSE(browsershm::readNewFrame(buffer.data(), buffer.size(), lastSequence).gotNewFrame);

  // A frame larger than the mapping must be refused by the writer (never corrupt).
  EXPECT_FALSE(browsershm::writeFrame(buffer.data(), buffer.size(), bgra.data(), 64, 64));

  // A header claiming more pixels than the mapping holds must be refused by the reader.
  browsershm::storeSeq(buffer.data(), 4);  // even, "new"
  const int bogusWidth = 4096;
  std::memcpy(buffer.data() + 4, &bogusWidth, sizeof(uint32_t));
  lastSequence = 0;
  EXPECT_FALSE(browsershm::readNewFrame(buffer.data(), buffer.size(), lastSequence).gotNewFrame);
}

// ---------------------------------------------------------------------------
// Add-request validation (the URL rides a CreateProcess command line)
// ---------------------------------------------------------------------------

TEST(BrowserSourceValidation, AcceptsNormalUrlsAndRejectsHostileOnes) {
  std::string error;
  EXPECT_TRUE(BrowserSourceHostAdapter::validateAddRequest("https://example.com/overlay?x=1", 1920,
                                                           1080, 30, error));
  EXPECT_TRUE(BrowserSourceHostAdapter::validateAddRequest("data:text/html,%3Ch1%3Ehi%3C/h1%3E",
                                                           640, 360, 30, error));
  EXPECT_TRUE(BrowserSourceHostAdapter::validateAddRequest("file:///C:/show/scoreboard.html", 1280,
                                                           720, 60, error));

  EXPECT_FALSE(BrowserSourceHostAdapter::validateAddRequest("", 1920, 1080, 30, error));
  EXPECT_FALSE(BrowserSourceHostAdapter::validateAddRequest("javascript:alert(1)", 1920, 1080, 30, error));
  EXPECT_FALSE(BrowserSourceHostAdapter::validateAddRequest("https://a.com/\" --evil", 1920, 1080,
                                                            30, error));
  EXPECT_FALSE(BrowserSourceHostAdapter::validateAddRequest("https://a.com/a b", 1920, 1080, 30, error));
  EXPECT_FALSE(BrowserSourceHostAdapter::validateAddRequest("https://a.com/x", 8, 8, 30, error));
  EXPECT_FALSE(BrowserSourceHostAdapter::validateAddRequest("https://a.com/x", 1920, 1080, 0, error));
  EXPECT_FALSE(BrowserSourceHostAdapter::validateAddRequest("https://a.com/x", 1920, 1080, 120, error));
}

// ---------------------------------------------------------------------------
// MediaCore command wiring + snapshot telemetry (stub modules, no host exe)
// ---------------------------------------------------------------------------

namespace {

// Point the adapter's host resolution at a nonexistent exe so tests exercise the
// loud spawn-failure path instead of launching real WebView2 processes (the dev
// build stages corevideo-browser-host.exe right next to the test binary).
void pinNonexistentHostExe() {
#ifdef _WIN32
  _putenv_s("COREVIDEO_BROWSER_HOST_PATH", "C:\\nonexistent\\corevideo-browser-host.exe");
#else
  setenv("COREVIDEO_BROWSER_HOST_PATH", "/nonexistent/corevideo-browser-host", 1);
#endif
}

const Json* findBrowserSource(const Json& snapshot, const std::string& id) {
  const Json* sources = snapshot.get("browserSources");
  if (sources == nullptr || !sources->isArray()) {
    return nullptr;
  }
  for (const auto& source : sources->asArray()) {
    if (source.getString("id") == id) {
      return &source;
    }
  }
  return nullptr;
}

}  // namespace

TEST(MediaCoreBrowserCommands, AddSurfacesSourceInSnapshotAndCaptureDevices) {
  pinNonexistentHostExe();
  MediaCore core(corevideo::modules::createStubModules());
  const Json snapshot = core.applyCommand(Json::Object{
      {"type", "browser-add"},
      {"url", "https://example.com/lower-third"},
      {"width", 1280},
      {"height", 720},
      {"fps", 30},
  });

  const Json* source = findBrowserSource(snapshot, "browser:1");
  ASSERT_NE(source, nullptr);
  EXPECT_EQ(source->getString("url"), "https://example.com/lower-third");
  EXPECT_EQ(static_cast<int>(source->getNumber("width")), 1280);
  EXPECT_EQ(static_cast<int>(source->getNumber("height")), 720);
  EXPECT_EQ(static_cast<int>(source->getNumber("fps")), 30);
  // No host exe has spawned yet (supervisor is async): the source must report a
  // non-live health, never a fake "live".
  EXPECT_NE(source->getString("health"), "live");

  // The source also presents as a capture device (vendor "browser" — the field
  // the wire JSON carries; the shell keys its Browser group on the id prefix +
  // vendor) for the Sources pickers.
  const Json* devices = snapshot.get("captureDevices");
  ASSERT_NE(devices, nullptr);
  bool found = false;
  for (const auto& device : devices->asArray()) {
    if (device.getString("id") == "browser:1") {
      found = true;
      EXPECT_EQ(device.getString("vendor"), "browser");
    }
  }
  EXPECT_TRUE(found);
}

TEST(MediaCoreBrowserCommands, RejectsInvalidUrlWithoutCreatingASource) {
  pinNonexistentHostExe();
  MediaCore core(corevideo::modules::createStubModules());
  const Json snapshot = core.applyCommand(Json::Object{
      {"type", "browser-add"},
      {"url", "javascript:alert(1)"},
  });
  const Json* sources = snapshot.get("browserSources");
  ASSERT_NE(sources, nullptr);
  EXPECT_TRUE(sources->asArray().empty());
}

TEST(MediaCoreBrowserCommands, RemoveDeletesTheSource) {
  pinNonexistentHostExe();
  MediaCore core(corevideo::modules::createStubModules());
  (void)core.applyCommand(Json::Object{
      {"type", "browser-add"},
      {"url", "https://example.com/a"},
  });
  const Json snapshot = core.applyCommand(Json::Object{
      {"type", "browser-remove"},
      {"browserId", "browser:1"},
  });
  EXPECT_EQ(findBrowserSource(snapshot, "browser:1"), nullptr);
}

TEST(MediaCoreBrowserCommands, AddDefaultsTo1080p30) {
  pinNonexistentHostExe();
  MediaCore core(corevideo::modules::createStubModules());
  const Json snapshot = core.applyCommand(Json::Object{
      {"type", "browser-add"},
      {"url", "https://example.com/full-canvas"},
  });
  const Json* source = findBrowserSource(snapshot, "browser:1");
  ASSERT_NE(source, nullptr);
  EXPECT_EQ(static_cast<int>(source->getNumber("width")), 1920);
  EXPECT_EQ(static_cast<int>(source->getNumber("height")), 1080);
  EXPECT_EQ(static_cast<int>(source->getNumber("fps")), 30);
}
