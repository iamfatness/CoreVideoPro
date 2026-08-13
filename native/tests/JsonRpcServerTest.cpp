#include "core/LockHoldGuardrail.h"
#include "core/MediaCore.h"
#include "rpc/Json.h"
#include "rpc/JsonRpcServer.h"

#include <gtest/gtest.h>

#include <optional>
#include <sstream>
#include <string>
#include <utility>

// Regression: the host (System.Text.Json default encoder) escapes non-ASCII and
// HTML-sensitive characters as \uXXXX. These appear in the larger media-core-sync
// (multiview layout labels) and zoom-media-spine-sync (roster) payloads — e.g. the
// "·" in a "Guest · Live" label. The parser used to throw "Unsupported JSON escape
// sequence" on \u, so every such request was answered with id="unknown", the bridge
// never matched the real id, and the request timed out at 4s. The parser must decode
// \u (including surrogate pairs) to UTF-8.
TEST(Json, ParsesUnicodeEscapesIncludingSurrogatePairs) {
  std::string error;

  // U+00B7 MIDDLE DOT -> UTF-8 0xC2 0xB7; U+002B '+'.
  auto basic = corevideo::rpc::Json::parse("{\"label\":\"Guest \\u00b7 Live \\u002B1\"}", &error);
  ASSERT_TRUE(basic.has_value()) << error;
  EXPECT_EQ(basic->getString("label"), std::string("Guest \xC2\xB7 Live +1"));

  // Surrogate pair: U+1F600 GRINNING FACE -> UTF-8 0xF0 0x9F 0x98 0x80.
  auto emoji = corevideo::rpc::Json::parse("{\"e\":\"\\uD83D\\uDE00\"}", &error);
  ASSERT_TRUE(emoji.has_value()) << error;
  EXPECT_EQ(emoji->getString("e"), std::string("\xF0\x9F\x98\x80"));
}

TEST(JsonRpcServer, EmitsHandshakeBeforeReadingInput) {
  corevideo::core::MediaCore mediaCore;
  corevideo::rpc::JsonRpcServer server(mediaCore);
  std::istringstream input;
  std::ostringstream output;

  server.run(input, output);

  std::string error;
  auto line = corevideo::rpc::Json::parse(output.str(), &error);
  ASSERT_TRUE(line.has_value()) << error;
  EXPECT_EQ(line->getString("id"), "handshake");
  EXPECT_TRUE(line->get("ok")->asBool());
  ASSERT_NE(line->get("profile"), nullptr);
#if COREVIDEO_WITH_D3D11
  EXPECT_EQ(line->get("profile")->getString("name"), "CoreVideo Pro Native Media Core");
  EXPECT_EQ(line->get("profile")->getString("renderer"), "d3d11");
#elif COREVIDEO_WITH_METAL
  EXPECT_EQ(line->get("profile")->getString("name"), "CoreVideo Pro Native Media Core");
  EXPECT_EQ(line->get("profile")->getString("renderer"), "metal");
#else
  EXPECT_EQ(line->get("profile")->getString("name"), "CoreVideo Pro Native Media Core Stub");
#endif
}

TEST(JsonRpcServer, MediaCoreSyncElapsedMsAdvancesRecordingProof) {
  corevideo::core::MediaCore mediaCore;
  corevideo::rpc::JsonRpcServer server(mediaCore);
  const auto response = server.handle(corevideo::rpc::Json::Object{
      {"id", "recording-proof-sync"},
      {"type", "media-core-sync"},
      {"elapsedMs", 1000},
      {"commands",
       corevideo::rpc::Json::Array{
           corevideo::rpc::Json::Object{
               {"type", "load-scene-graph"},
               {"sceneId", "recording-proof"},
               {"routes", corevideo::rpc::Json::Array{
                              corevideo::rpc::Json::Object{
                                  {"routeId", "program"},
                                  {"mode", "fixed"},
                                  {"audioRole", "mix"},
                                  {"participantId", "speaker-1"},
                              },
                          }},
           },
           corevideo::rpc::Json::Object{
               {"type", "start-program-output"},
               {"destinations", corevideo::rpc::Json::Array{"recording"}},
               {"isoParticipantIds", corevideo::rpc::Json::Array{"speaker-1"}},
           },
           corevideo::rpc::Json::Object{
               {"type", "set-recording-targets"},
               {"targetFolder", "Recordings/CoreVideo Pro/native-proof"},
               {"filenamePrefix", "alpha-proof"},
               {"format", "mp4"},
               {"quality", "high"},
               {"isoParticipantIds", corevideo::rpc::Json::Array{"speaker-1"}},
           },
           corevideo::rpc::Json::Object{
               {"type", "start-recording-session"},
               {"sessionId", "alpha-proof-speaker-1"},
               {"startedAtMs", 1000},
           },
       }},
  });

  ASSERT_TRUE(response.get("ok")->asBool());
  const auto* recording = response.get("snapshot")->get("recording");
  ASSERT_NE(recording, nullptr);
  EXPECT_EQ(recording->getString("status"), "recording");
  const auto* proof = recording->get("proof");
  ASSERT_NE(proof, nullptr);
  EXPECT_GE(proof->get("durationMs")->asNumber(), 33);
  EXPECT_GE(proof->get("programFrameCount")->asNumber(), 1);
  EXPECT_GE(proof->get("isoFrameCount")->asNumber(), 1);
  EXPECT_TRUE(proof->get("metadataValid")->asBool());
  EXPECT_EQ(proof->getString("containerFormat"), "mp4");
}

TEST(JsonRpcServer, HandlesMediaCoreSyncRequestEnvelope) {
  corevideo::core::MediaCore mediaCore;
  corevideo::rpc::JsonRpcServer server(mediaCore);
  const auto response = server.handle(corevideo::rpc::Json::Object{
      {"id", "core-sync-1"},
      {"type", "media-core-sync"},
      {"elapsedMs", 66},
      {"commands",
       corevideo::rpc::Json::Array{
           corevideo::rpc::Json::Object{
               {"type", "start-program-output"},
               {"destinations", corevideo::rpc::Json::Array{"recording"}},
               {"isoParticipantIds", corevideo::rpc::Json::Array{}},
           },
       }},
  });

  EXPECT_EQ(response.getString("id"), "core-sync-1");
  EXPECT_TRUE(response.get("ok")->asBool());
  EXPECT_EQ(response.getString("type"), "media-core-sync");
  ASSERT_NE(response.get("snapshot"), nullptr);
  EXPECT_TRUE(response.get("snapshot")->get("active")->asBool());
  EXPECT_EQ(response.get("snapshot")->getString("meetingState"), "idle");
  EXPECT_EQ(response.get("snapshot")->getString("breakoutRoomId"), "main");
  EXPECT_EQ(response.get("snapshot")->getString("breakoutRoomName"), "Main room");
}

TEST(JsonRpcServer, SimulatesBreakoutRoomChangeOnSyncCommand) {
  corevideo::core::MediaCore mediaCore;
  corevideo::rpc::JsonRpcServer server(mediaCore);
  const auto response = server.handle(corevideo::rpc::Json::Object{
      {"id", "core-sync-room"},
      {"type", "media-core-sync"},
      {"elapsedMs", 120},
      {"commands",
       corevideo::rpc::Json::Array{
           corevideo::rpc::Json::Object{
               {"type", "simulate-breakout-room-change"},
               {"breakoutRoomId", "customer-panel"},
               {"breakoutRoomName", "Customer panel"},
           },
           corevideo::rpc::Json::Object{
               {"type", "start-program-output"},
               {"destinations", corevideo::rpc::Json::Array{"recording"}},
               {"isoParticipantIds", corevideo::rpc::Json::Array{}},
           },
       }},
  });

  ASSERT_NE(response.get("snapshot"), nullptr);
  EXPECT_EQ(response.get("snapshot")->getString("breakoutRoomId"), "customer-panel");
  EXPECT_EQ(response.get("snapshot")->getString("breakoutRoomName"), "Customer panel");
}

TEST(JsonRpcServer, PreservesIdAndAcksStartProgramOutput) {
  corevideo::core::MediaCore mediaCore;
  corevideo::rpc::JsonRpcServer server(mediaCore);
  const auto response = server.handle(corevideo::rpc::Json::Object{
      {"id", "agent-a-1"},
      {"type", "start-program-output"},
      {"destinations", corevideo::rpc::Json::Array{"recording", "rtmp"}},
      {"isoParticipantIds", corevideo::rpc::Json::Array{"participant-1"}},
  });

  EXPECT_EQ(response.getString("id"), "agent-a-1");
  EXPECT_TRUE(response.get("ok")->asBool());
  ASSERT_NE(response.get("snapshot"), nullptr);
  const auto* health = response.get("snapshot")->get("health");
  ASSERT_NE(health, nullptr);
  EXPECT_EQ(health->getString("status"), "live");
  EXPECT_GE(health->get("encodedFrameCount")->asNumber(), 1);
}

TEST(JsonRpcServer, ParsesLineDelimitedJsonRequests) {
  corevideo::core::MediaCore mediaCore;
  corevideo::rpc::JsonRpcServer server(mediaCore);
  std::istringstream input("{\"id\":\"one\",\"type\":\"get-output-health\"}\n");
  std::ostringstream output;

  server.run(input, output);

  std::istringstream lines(output.str());
  std::string handshakeLine;
  ASSERT_TRUE(std::getline(lines, handshakeLine));
  // The threaded server's render thread may interleave frame events between
  // the handshake and the command response, so scan for the line that echoes
  // our request id instead of assuming it is the next line.
  std::optional<corevideo::rpc::Json> response;
  std::string line;
  while (std::getline(lines, line)) {
    auto parsed = corevideo::rpc::Json::parse(line);
    if (parsed.has_value() && parsed->getString("id") == "one") {
      response = std::move(parsed);
      break;
    }
  }
  ASSERT_TRUE(response.has_value());
  EXPECT_EQ(response->getString("id"), "one");
  EXPECT_TRUE(response->get("ok")->asBool());
  const auto* health = response->get("health");
  ASSERT_NE(health, nullptr);
  ASSERT_NE(health->get("zoom"), nullptr);
  EXPECT_EQ(health->get("zoom")->get("readiness")->getString("status"), "ready");
  EXPECT_EQ(health->get("zoom")->get("readiness")->getString("mode"), "stub");
  EXPECT_TRUE(health->get("zoom")->get("evidence")->get("synthetic")->asBool());
}

TEST(JsonRpcServer, HandlesZoomMediaSpineSyncRequest) {
  corevideo::core::MediaCore mediaCore;
  corevideo::rpc::JsonRpcServer server(mediaCore);
  const auto response = server.handle(corevideo::rpc::Json::Object{
      {"id", "zoom-spine-1"},
      {"type", "zoom-media-spine-sync"},
      {"elapsedMs", 66},
      {"payload",
       corevideo::rpc::Json::Object{
           {"readiness",
            corevideo::rpc::Json::Object{
                {"status", "ready"},
                {"platform", "windows"},
                {"sdkVersion", "7.0.5"},
                {"checks", corevideo::rpc::Json::Array{}},
            }},
           {"participants",
            corevideo::rpc::Json::Array{
                corevideo::rpc::Json::Object{
                    {"sdkUserId", "sdk-presenter"},
                    {"displayName", "Andre Wallace"},
                    {"role", "panelist"},
                    {"videoOn", true},
                    {"muted", false},
                    {"talking", true},
                    {"sharingScreen", false},
                    {"audioLevel", 80},
                    {"networkQuality", "good"},
                },
            }},
           {"subscriptions",
            corevideo::rpc::Json::Array{
                corevideo::rpc::Json::Object{{"participantId", "sdk-presenter"}, {"kind", "participant-video"}, {"purpose", "program"}, {"priority", 1}},
                corevideo::rpc::Json::Object{{"participantId", "sdk-presenter"}, {"kind", "participant-audio"}, {"purpose", "mix"}, {"priority", 2}},
            }},
           {"recording",
            corevideo::rpc::Json::Object{
                {"targetFolder", "Recordings"},
                {"filenamePrefix", "zoom-spine"},
                {"format", "mp4"},
                {"quality", "high"},
                {"isoParticipantIds", corevideo::rpc::Json::Array{"sdk-presenter"}},
            }},
           {"blocked", false},
           {"warnings", corevideo::rpc::Json::Array{}},
           {"summary", "1 Zoom participant, 1 video and 1 audio raw Zoom subscriptions planned."},
       }},
  });

  EXPECT_EQ(response.getString("id"), "zoom-spine-1");
  EXPECT_TRUE(response.get("ok")->asBool());
  EXPECT_EQ(response.getString("type"), "zoom-media-spine-sync");
  const auto* spineSnapshot = response.get("spineSnapshot");
  ASSERT_NE(spineSnapshot, nullptr);
  EXPECT_EQ(spineSnapshot->getString("meetingState"), "in-meeting");
  EXPECT_EQ(spineSnapshot->getString("activeSpeakerId"), "sdk-presenter");
  EXPECT_EQ(spineSnapshot->get("recording")->get("evidence")->get("subscribedVideoFeeds")->asNumber(), 1);
  EXPECT_GE(spineSnapshot->get("recording")->get("evidence")->get("programFramesWritten")->asNumber(), 1);
}

TEST(JsonRpcServer, HandlesZoomLifecycleRequests) {
  corevideo::core::MediaCore mediaCore;
  corevideo::rpc::JsonRpcServer server(mediaCore);

  const auto joined = server.handle(corevideo::rpc::Json::Object{
      {"id", "zoom-join-1"},
      {"type", "zoom-join"},
      {"payload",
       corevideo::rpc::Json::Object{
           {"meetingUrl", "https://zoom.us/j/123456789"},
           {"displayName", "Operator"},
           {"webinar", false},
       }},
  });
  EXPECT_EQ(joined.getString("id"), "zoom-join-1");
  EXPECT_TRUE(joined.get("ok")->asBool());
  EXPECT_EQ(joined.getString("type"), "zoom-join");
  ASSERT_NE(joined.get("snapshot"), nullptr);
  EXPECT_EQ(joined.get("snapshot")->getString("meetingState"), "in_meeting");
  EXPECT_EQ(joined.get("snapshot")->getString("activeSpeakerId"), "operator-1");
  ASSERT_TRUE(joined.get("snapshot")->get("participants")->asArray().size() >= 2);
  EXPECT_EQ(joined.get("snapshot")->get("participants")->asArray()[0].getString("displayName"), "Operator");

  const auto snapshot = server.handle(corevideo::rpc::Json::Object{
      {"id", "zoom-snapshot-1"},
      {"type", "zoom-snapshot"},
  });
  EXPECT_EQ(snapshot.getString("id"), "zoom-snapshot-1");
  EXPECT_TRUE(snapshot.get("ok")->asBool());
  EXPECT_EQ(snapshot.getString("type"), "zoom-snapshot");
  EXPECT_EQ(snapshot.get("snapshot")->getString("meetingState"), "in_meeting");
  EXPECT_EQ(snapshot.get("snapshot")->get("readiness")->getString("status"), "ready");
  EXPECT_EQ(snapshot.get("snapshot")->get("readiness")->getString("mode"), "stub");
  EXPECT_FALSE(snapshot.get("snapshot")->get("readiness")->get("sdkAvailable")->asBool());
  EXPECT_TRUE(snapshot.get("snapshot")->get("evidence")->get("synthetic")->asBool());
  EXPECT_EQ(snapshot.get("snapshot")->get("evidence")->get("participantCount")->asNumber(), 2);

  // Capture-off dispatch: zoom-stop-capture stops raw media but is NOT a
  // leave — the meeting stays joined and the participants stay in the snapshot.
  const auto stoppedCapture = server.handle(corevideo::rpc::Json::Object{
      {"id", "zoom-stop-capture-1"},
      {"type", "zoom-stop-capture"},
  });
  EXPECT_EQ(stoppedCapture.getString("id"), "zoom-stop-capture-1");
  EXPECT_TRUE(stoppedCapture.get("ok")->asBool());
  EXPECT_EQ(stoppedCapture.getString("type"), "zoom-stop-capture");
  EXPECT_EQ(stoppedCapture.get("snapshot")->getString("meetingState"), "in_meeting");
  EXPECT_FALSE(stoppedCapture.get("snapshot")->get("participants")->asArray().empty());

  const auto left = server.handle(corevideo::rpc::Json::Object{
      {"id", "zoom-leave-1"},
      {"type", "zoom-leave"},
  });
  EXPECT_EQ(left.getString("id"), "zoom-leave-1");
  EXPECT_TRUE(left.get("ok")->asBool());
  EXPECT_EQ(left.getString("type"), "zoom-leave");
  EXPECT_EQ(left.get("snapshot")->getString("meetingState"), "idle");
  EXPECT_TRUE(left.get("snapshot")->get("participants")->asArray().empty());
}

TEST(JsonRpcServer, HandlesCaptureDeviceBridgeRequests) {
  corevideo::core::MediaCore mediaCore;
  corevideo::rpc::JsonRpcServer server(mediaCore);

  const auto listed = server.handle(corevideo::rpc::Json::Object{
      {"id", "capture-list-1"},
      {"type", "list-capture-devices"},
  });
  EXPECT_EQ(listed.getString("id"), "capture-list-1");
  EXPECT_TRUE(listed.get("ok")->asBool());
  EXPECT_EQ(listed.getString("type"), "capture-devices");
  ASSERT_NE(listed.get("devices"), nullptr);
  EXPECT_GE(listed.get("devices")->asArray().size(), 2);
  const auto deckLinkId = listed.get("devices")->asArray()[0].getString("id");
  const auto ajaId = listed.get("devices")->asArray()[1].getString("id");

  const auto selected = server.handle(corevideo::rpc::Json::Object{
      {"id", "capture-select-1"},
      {"type", "select-capture-input"},
      {"payload", corevideo::rpc::Json::Object{{"deviceId", deckLinkId}, {"inputId", "hdmi-1"}}},
  });
  EXPECT_TRUE(selected.get("ok")->asBool());
  EXPECT_EQ(selected.get("devices")->asArray()[0].getString("selectedInputId"), "hdmi-1");

  const auto offset = server.handle(corevideo::rpc::Json::Object{
      {"id", "capture-offset-1"},
      {"type", "set-capture-audio-sync-offset"},
      {"payload", corevideo::rpc::Json::Object{{"deviceId", deckLinkId}, {"offsetMs", 1200}}},
  });
  EXPECT_TRUE(offset.get("ok")->asBool());
  EXPECT_EQ(offset.get("devices")->asArray()[0].get("audioSyncOffsetMs")->asNumber(), 500);

  const auto connected = server.handle(corevideo::rpc::Json::Object{
      {"id", "capture-connect-1"},
      {"type", "connect-capture-device"},
      {"payload", corevideo::rpc::Json::Object{{"deviceId", ajaId}}},
  });
  EXPECT_TRUE(connected.get("ok")->asBool());
  EXPECT_EQ(connected.get("devices")->asArray()[1].getString("connectionState"), "connected");
}

// A1 regression (owner-reported "Open controls never shows a plugin UI"): the
// shell sends open-vst-editor as a TOP-LEVEL request. handle() used to reject
// it with "Unsupported native media-core command" — and the shell discarded the
// ok:false response, so the defect was a silent no-op. The request must route
// into MediaCore::handleCommand and the resolution failure must surface LOUDLY
// in pluginHost.serve.lastError (never silence).
// (scan-vst-plugins rides the same routing branch; it is not driven end-to-end
// here because a real scan would spawn probe processes from a unit test.)
TEST(JsonRpcServer, TopLevelOpenVstEditorRoutesToMediaCore) {
  corevideo::core::MediaCore mediaCore;
  corevideo::rpc::JsonRpcServer server(mediaCore);

  // A non-"vst:" selection fails in openVstPluginEditor BEFORE any scan
  // auto-kick or host spawn — deterministic and thread-free for a unit test.
  const auto response = server.handle(corevideo::rpc::Json::Object{
      {"id", "open-editor-1"},
      {"type", "open-vst-editor"},
      {"selection", "gate"},
  });
  EXPECT_EQ(response.getString("id"), "open-editor-1");
  ASSERT_NE(response.get("ok"), nullptr);
  EXPECT_TRUE(response.get("ok")->asBool()) << response.stringify();
  ASSERT_NE(response.get("snapshot"), nullptr);

  const auto* mixSession = response.get("snapshot")->get("audioMixSession");
  ASSERT_NE(mixSession, nullptr);
  const auto* pluginHost = mixSession->get("pluginHost");
  ASSERT_NE(pluginHost, nullptr);
  const auto* serve = pluginHost->get("serve");
  ASSERT_NE(serve, nullptr);
  EXPECT_NE(serve->getString("lastError").find("cannot open controls"), std::string::npos)
      << serve->stringify();
}

// Phase 2 increment 6: the coreMutex hold-duration guardrail must be wired into
// the live server loop — handling a command under coreMutex records a hold at
// the sanctioned "cmd.handle" site (and the render thread at
// "render.display-tick"). This also runs the whole multi-threaded server under
// the native-stub-tsan CI job, exercising the guardrail registry concurrently
// with the render/audio/zoom-pump threads.
TEST(JsonRpcServer, RecordsCoreLockHoldGuardrailStatsForHandledCommands) {
  corevideo::core::LockHoldGuardrail::resetForTest();
  corevideo::core::MediaCore mediaCore;
  corevideo::rpc::JsonRpcServer server(mediaCore);
  std::istringstream input(
      "{\"id\":\"p1\",\"type\":\"ping\"}\n"
      "{\"id\":\"s1\",\"type\":\"media-core-sync\",\"elapsedMs\":33}\n");
  std::ostringstream output;

  server.run(input, output);

  const auto stats = corevideo::core::LockHoldGuardrail::statsForSite("cmd.handle");
  EXPECT_GE(stats.holds, 2u);
  // No hard assertion on overBudget: the budget is telemetry (rate-capped
  // warnings), never a test failure — especially under TSan's slowdown.
}

// Under a command backlog the loop coalesces stale media-core-sync batches
// (skips the expensive apply for a sync that already has a newer one queued) so
// it reaches level-triggered control commands like stop-recording promptly. The
// non-negotiable invariant coalescing must never break: EVERY request still gets
// exactly one response echoing its id (a dropped response = a bridge timeout).
TEST(JsonRpcServer, CoalescedSyncBacklogStillAnswersEveryRequest) {
  corevideo::core::MediaCore mediaCore;
  corevideo::rpc::JsonRpcServer server(mediaCore);

  // A burst of syncs (distinct ids) so several are queued at once — that is when
  // coalescing may fire. Whether or not it does, all must be answered.
  std::string in;
  const int kSyncs = 24;
  for (int i = 0; i < kSyncs; ++i) {
    in += "{\"id\":\"sync-" + std::to_string(i) +
          "\",\"type\":\"media-core-sync\",\"elapsedMs\":33}\n";
  }
  std::istringstream input(in);
  std::ostringstream output;

  server.run(input, output);

  // Every sync-<i> id must appear at least once in the responses.
  const std::string out = output.str();
  for (int i = 0; i < kSyncs; ++i) {
    const std::string needle = "\"sync-" + std::to_string(i) + "\"";
    EXPECT_NE(out.find(needle), std::string::npos)
        << "no response for sync-" << i << " (coalescing dropped a request)";
  }
}

// Last-writer-wins after coalescing: the NEWEST sync is never coalesced (nothing
// is queued behind it), so its control command is fully applied even when every
// earlier sync in the burst was skipped. Here a burst of "not recording" syncs is
// followed by a final "start recording" sync; its response must show recording
// active — proving the newest control state takes effect through the backlog.
TEST(JsonRpcServer, CoalescedSyncAppliesNewestControlState) {
  corevideo::core::MediaCore mediaCore;
  corevideo::rpc::JsonRpcServer server(mediaCore);

  auto stopSync = [](const std::string& id) {
    return "{\"id\":\"" + id +
           "\",\"type\":\"media-core-sync\",\"elapsedMs\":33,\"commands\":["
           "{\"type\":\"stop-recording-session\",\"reason\":\"idle\"}]}\n";
  };
  const std::string startSync =
      "{\"id\":\"s-start\",\"type\":\"media-core-sync\",\"elapsedMs\":33,\"commands\":["
      "{\"type\":\"set-recording-targets\",\"targetFolder\":\"rec\",\"filenamePrefix\":\"show\","
      "\"format\":\"mp4\",\"quality\":\"medium\"},"
      "{\"type\":\"start-recording-session\",\"sessionId\":\"show-program\",\"targetFolder\":\"rec\","
      "\"filenamePrefix\":\"show\",\"format\":\"mp4\",\"quality\":\"medium\"}]}\n";

  std::string in;
  for (int i = 0; i < 12; ++i) {
    in += stopSync("s" + std::to_string(i));
  }
  in += startSync;  // newest sync: never coalesced, must take effect
  std::istringstream input(in);
  std::ostringstream output;

  server.run(input, output);

  // Read the final start sync's OWN response and confirm recording is active.
  const std::string out = output.str();
  const auto pos = out.rfind("\"s-start\"");
  ASSERT_NE(pos, std::string::npos);
  const auto lineStart = out.rfind('\n', pos);
  const auto lineEnd = out.find('\n', pos);
  const std::string line = out.substr(
      lineStart == std::string::npos ? 0 : lineStart + 1,
      (lineEnd == std::string::npos ? out.size() : lineEnd) -
          (lineStart == std::string::npos ? 0 : lineStart + 1));
  std::string error;
  auto response = corevideo::rpc::Json::parse(line, &error);
  ASSERT_TRUE(response.has_value()) << error << " line=" << line;
  const auto* snapshot = response->get("snapshot");
  ASSERT_NE(snapshot, nullptr);
  const auto* recording = snapshot->get("recording");
  ASSERT_NE(recording, nullptr) << "newest start-recording sync was not applied";
  const auto* active = recording->get("active");
  ASSERT_NE(active, nullptr);
  EXPECT_TRUE(active->asBool());
}
