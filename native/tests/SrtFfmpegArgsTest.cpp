#include "modules/SrtFfmpegArgs.h"

#include <gtest/gtest.h>

#include <string>

using corevideo::modules::SrtEndpointConfig;
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
