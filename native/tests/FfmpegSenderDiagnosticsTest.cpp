#include "modules/FfmpegSenderDiagnostics.h"

#include <gtest/gtest.h>

namespace {
using namespace corevideo::modules;

// The real stderr FFmpeg wrote on the owner's machine, 2026-09-12 13:55:12, when
// a stream that had been blamed on the stream key was actually refused by the
// destination. The key in it is the one that WORKED two minutes later.
constexpr const char* kLiveFailureStderr =
    "[aist#1:0/pcm_f32le @ 000001fc9a5767c0] Guessed Channel Layout: stereo\n"
    "[out#0/flv @ 000001fc9a5d4f00] Error opening output "
    "rtmp://a.rtmp.youtube.com/live2/z3vb-secretkey-bku: I/O error\n"
    "Error opening output file rtmp://a.rtmp.youtube.com/live2/z3vb-secretkey-bku.\n"
    "Error opening output files: I/O error\n";

TEST(FfmpegSenderDiagnostics, TheStreamKeyNeverSurvivesIntoADiagnostic) {
  const auto redacted =
      redactFfmpegDiagnostics(kLiveFailureStderr, "z3vb-secretkey-bku", "");

  // The whole reason this function exists: lastError reaches /snapshot and the
  // support bundle, and FFmpeg echoes the full output URL, key included.
  EXPECT_EQ(redacted.find("z3vb-secretkey-bku"), std::string::npos);
  EXPECT_NE(redacted.find("<stream-key>"), std::string::npos);
  // The HOST must survive - it is the diagnostic, and it is not a secret.
  EXPECT_NE(redacted.find("a.rtmp.youtube.com"), std::string::npos);
  // And the cause itself must survive.
  EXPECT_NE(redacted.find("I/O error"), std::string::npos);
}

TEST(FfmpegSenderDiagnostics, AnSrtPassphraseIsRedactedToo) {
  const auto redacted = redactFfmpegDiagnostics(
      "Error opening output srt://host:9000?passphrase=hunter2supersecret: I/O error",
      "", "hunter2supersecret");

  EXPECT_EQ(redacted.find("hunter2supersecret"), std::string::npos);
  EXPECT_NE(redacted.find("<passphrase>"), std::string::npos);
}

// A short key would otherwise match inside ordinary words and shred the message.
TEST(FfmpegSenderDiagnostics, AnImplausiblyShortSecretIsNotUsedAsAPattern) {
  const auto redacted = redactFfmpegDiagnostics("Error opening output: I/O error", "or", "");
  EXPECT_EQ(redacted, "Error opening output: I/O error");
}

TEST(FfmpegSenderDiagnostics, TheTailIsFlattenedToOneBoundedLine) {
  const auto tail = flattenFfmpegStderrTail(kLiveFailureStderr, 2048);

  EXPECT_EQ(tail.find('\n'), std::string::npos);
  EXPECT_NE(tail.find("Error opening output files: I/O error"), std::string::npos);
}

TEST(FfmpegSenderDiagnostics, OnlyTheTailIsKeptWhenTheLogIsLong) {
  std::string noisy(8192, 'x');
  noisy += "\nthe last words\n";

  const auto tail = flattenFfmpegStderrTail(noisy, 256);

  EXPECT_LE(tail.size(), static_cast<std::size_t>(256));
  EXPECT_NE(tail.find("the last words"), std::string::npos);
}

// The whole point of the change: the operator sentence must carry FFmpeg's own
// words. Without them "FFmpeg stdin write failed" is undiagnosable and reads as
// a credential problem.
TEST(FfmpegSenderDiagnostics, TheOperatorSentenceCarriesFfmpegsOwnWords) {
  const auto described = describeFfmpegSenderFailure(
      "FFmpeg process exited before accepting program frames. Exit code 1.",
      "Error opening output files: I/O error");

  EXPECT_NE(described.find("Exit code 1."), std::string::npos);
  EXPECT_NE(described.find("I/O error"), std::string::npos);
}

TEST(FfmpegSenderDiagnostics, AnEmptyTailLeavesTheGenericSentenceAlone) {
  const std::string generic = "FFmpeg stdin write failed.";
  EXPECT_EQ(describeFfmpegSenderFailure(generic, ""), generic);
  EXPECT_EQ(describeFfmpegSenderFailure(generic, "   \n  "), generic);
}
}  // namespace
