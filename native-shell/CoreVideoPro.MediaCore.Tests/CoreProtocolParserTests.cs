using System.Text.Json;
using CoreVideoPro.MediaCore.Models;
using CoreVideoPro.MediaCore.Services;
using Xunit;

namespace CoreVideoPro.MediaCore.Tests;

public sealed class CoreProtocolParserTests
{
    [Fact]
    public void DistinguishesUnsolicitedZoomVideoFrameEventsFromResponses()
    {
        var pixels = new byte[8];
        pixels[0] = 255;
        pixels[1] = 255;
        pixels[2] = 255;
        pixels[3] = 255;
        pixels[4] = 0;
        pixels[5] = 0;
        pixels[6] = 0;
        pixels[7] = 255;
        var base64 = Convert.ToBase64String(pixels);
        var eventLine = $$"""
            {
              "type": "zoom-video-frame",
              "frame": {
                "participantId": "42",
                "width": 2,
                "height": 1,
                "frameId": 7,
                "bgraBase64": "{{base64}}"
              }
            }
            """;

        var frameEvent = CoreProtocolParser.TryParseEvent(eventLine);
        Assert.NotNull(frameEvent);
        Assert.Equal("42", frameEvent!.Frame.ParticipantId);
        Assert.Equal(7, frameEvent.Frame.FrameId);
        Assert.Equal(2, frameEvent.Frame.Width);
        Assert.Equal(1, frameEvent.Frame.Height);
        Assert.Equal(pixels, frameEvent.Frame.Bgra);
        Assert.Null(CoreProtocolParser.TryParseResponse(eventLine));
        Assert.Null(CoreProtocolParser.TryParseEvent("""{"id":"core-1","ok":true,"type":"ping"}"""));
    }

    [Fact]
    public void ParsesMultiviewSharedTextureEventWithTiles()
    {
        var line = """
            {
              "type": "multiview-shared-texture",
              "texture": { "sharedHandleHex": "0x1A2B3C", "width": 1920, "height": 1080, "format": "B8G8R8A8_UNORM", "frameNumber": 42 },
              "canvasWidth": 1920,
              "canvasHeight": 1080,
              "tiles": [
                { "sourceId": "zoom:p-1", "participantId": "p-1", "slot": 0, "label": "Host", "activeSpeaker": true, "x": 0.0, "y": 0.0, "w": 0.5, "h": 0.5 },
                { "sourceId": "capture:cam-1", "participantId": "", "slot": 1, "label": "Cam", "activeSpeaker": false, "x": 0.5, "y": 0.0, "w": 0.5, "h": 0.5 }
              ]
            }
            """;

        var multiviewEvent = CoreProtocolParser.TryParseMultiviewSharedTextureEvent(line);
        Assert.NotNull(multiviewEvent);
        var multiview = multiviewEvent!.Multiview;
        Assert.True(multiview.IsValid);
        Assert.Equal(1920, multiview.CanvasWidth);
        Assert.Equal(1080, multiview.CanvasHeight);
        Assert.Equal(42, multiview.Texture.FrameNumber);
        Assert.Equal(2, multiview.Tiles.Count);

        var host = multiview.Tiles[0];
        Assert.Equal("zoom:p-1", host.SourceId);
        Assert.Equal("p-1", host.ParticipantId);
        Assert.True(host.ActiveSpeaker);
        Assert.Equal(0.5, host.W, 3);

        var cam = multiview.Tiles[1];
        Assert.Equal("capture:cam-1", cam.SourceId);
        Assert.False(cam.ActiveSpeaker);
        Assert.Equal(0.5, cam.X, 3);

        // A program-shared-texture event must not be misread as multiview, and vice versa.
        Assert.Null(CoreProtocolParser.TryParseMultiviewSharedTextureEvent(
            """{"type":"program-shared-texture","texture":{"sharedHandleHex":"0x1","width":16,"height":9}}"""));
        Assert.Null(CoreProtocolParser.TryParseProgramSharedTextureEvent(line));
    }

    [Fact]
    public void RejectsZoomVideoFramesWithMismatchedBgraLength()
    {
        var base64 = Convert.ToBase64String(new byte[4]);
        var line = $$"""
            {
              "type": "zoom-video-frame",
              "frame": {
                "participantId": "p1",
                "width": 2,
                "height": 1,
                "frameId": 1,
                "bgraBase64": "{{base64}}"
              }
            }
            """;
        Assert.Null(CoreProtocolParser.TryParseEvent(line));
    }

    [Fact]
    public void RejectsMessagesThatCarryRequestId()
    {
        var pixels = new byte[4];
        var base64 = Convert.ToBase64String(pixels);
        var line = $$"""
            {
              "id": "x",
              "type": "zoom-video-frame",
              "frame": {
                "participantId": "p1",
                "width": 1,
                "height": 1,
                "frameId": 1,
                "bgraBase64": "{{base64}}"
              }
            }
            """;
        Assert.Null(CoreProtocolParser.TryParseEvent(line));
    }

    [Fact]
    public void ParsesZoomMediaSpineSyncResponse()
    {
        using var document = System.Text.Json.JsonDocument.Parse("""
            {
              "id": "core-9",
              "ok": true,
              "type": "zoom-media-spine-sync",
              "spineSnapshot": {
                "meetingState": "in-meeting",
                "sdkVersion": "zoom-engine",
                "participantCount": 1,
                "activeSpeakerId": "operator-1",
                "participants": [
                  {
                    "sdkUserId": "operator-1",
                    "displayName": "Operator",
                    "role": "guest",
                    "videoOn": true,
                    "muted": false,
                    "talking": true,
                    "sharingScreen": false,
                    "audioLevel": 70,
                    "networkQuality": "good"
                  }
                ],
                "subscriptions": [],
                "warnings": [],
                "events": ["zoom-media-spine-sync accepted by Node stub core."]
              }
            }
            """);

        var snapshot = CoreProtocolParser.TryParseZoomMediaSpineSnapshot(document);
        Assert.NotNull(snapshot);
        Assert.Equal("in-meeting", snapshot!.MeetingState);
        Assert.Equal("operator-1", snapshot.ActiveSpeakerId);
        Assert.Single(snapshot.Participants);
        Assert.Equal("Operator", snapshot.Participants[0].DisplayName);
    }

    [Fact]
    public void ErrorMessageIncludesDetailsWarningsAndEvents()
    {
        using var document = System.Text.Json.JsonDocument.Parse("""
            {
              "id": "core-10",
              "ok": false,
              "type": "media-core-sync",
              "error": {
                "message": "start-program-output failed.",
                "detail": "ffmpeg.exe was not found in C:\\ffmpeg\\bin.",
                "code": "rtmp-output-unavailable",
                "warnings": ["RTMP sender was not started."]
              },
              "snapshot": {
                "warnings": ["Stream destination rtmp is offline."],
                "events": ["start-program-output rejected by media-core."]
              }
            }
            """);

        var message = CoreProtocolParser.TryParseErrorMessage(document);

        Assert.Equal(
            "start-program-output failed. ffmpeg.exe was not found in C:\\ffmpeg\\bin. rtmp-output-unavailable RTMP sender was not started. Stream destination rtmp is offline. start-program-output rejected by media-core.",
            message);
    }

    [Fact]
    public void ErrorMessageIncludesStringErrorPayload()
    {
        using var document = System.Text.Json.JsonDocument.Parse("""
            {
              "id": "core-11",
              "ok": false,
              "type": "media-core-sync",
              "error": "start-program-output failed: RTMP connection refused"
            }
            """);

        var message = CoreProtocolParser.TryParseErrorMessage(document);

        Assert.Equal("start-program-output failed: RTMP connection refused", message);
    }

    [Fact]
    public void ErrorMessageIncludesNativeHintsStderrAndErrors()
    {
        using var document = System.Text.Json.JsonDocument.Parse("""
            {
              "id": "core-12",
              "ok": false,
              "type": "media-core-sync",
              "error": {
                "message": "start-program-output failed.",
                "reason": "RTMP sender exited.",
                "hint": "Check stream key and endpoint.",
                "stderr": "Connection refused",
                "errors": ["ffmpeg exited with code 1"]
              }
            }
            """);

        var message = CoreProtocolParser.TryParseErrorMessage(document);

        Assert.Equal(
            "start-program-output failed. RTMP sender exited. Check stream key and endpoint. Connection refused ffmpeg exited with code 1",
            message);
    }

    [Fact]
    public void ErrorMessageIncludesNestedOutputSenderSessionDetails()
    {
        using var document = System.Text.Json.JsonDocument.Parse("""
            {
              "id": "core-13",
              "ok": false,
              "type": "media-core-sync",
              "error": {
                "message": "start-program-output failed."
              },
              "snapshot": {
                "outputSenderSession": {
                  "status": "warning",
                  "warnings": ["Stream destination rtmp is offline."],
                  "senders": [
                    {
                      "destination": "rtmp",
                      "status": "warning",
                      "warning": "RTMP ingest rejected credentials.",
                      "lastError": "RTMP ingest rejected credentials."
                    }
                  ]
                }
              }
            }
            """);

        var message = CoreProtocolParser.TryParseErrorMessage(document);

        Assert.Equal(
            "start-program-output failed. Stream destination rtmp is offline. RTMP sender warning: RTMP ingest rejected credentials.",
            message);
    }

    [Fact]
    public void DoesNotParseResponseLineAsEvent()
    {
        var responseLine = """{"id":"1","ok":true,"type":"ping"}""";
        Assert.Null(CoreProtocolParser.TryParseEvent(responseLine));
        Assert.NotNull(CoreProtocolParser.TryParseResponse(responseLine));
    }

    [Fact]
    public void ParsesUnsolicitedProgramFramePreviewEvent()
    {
        var pixels = new byte[4];
        pixels[0] = 12;
        pixels[1] = 34;
        pixels[2] = 56;
        pixels[3] = 255;
        var base64 = Convert.ToBase64String(pixels);
        var eventLine = $$"""
            {
              "type": "program-frame-preview",
              "preview": {
                "frameNumber": 9,
                "width": 1,
                "height": 1,
                "renderPlanId": "plan-9",
                "renderer": "d3d11",
                "health": "live",
                "pixelFormat": "bgra",
                "bgraBase64": "{{base64}}"
              }
            }
            """;

        var previewEvent = CoreProtocolParser.TryParseProgramFramePreviewEvent(eventLine);
        Assert.NotNull(previewEvent);
        Assert.Equal(9, previewEvent!.Preview.FrameNumber);
        Assert.Equal(1, previewEvent.Preview.Width);
        Assert.Equal(1, previewEvent.Preview.Height);
        Assert.Equal("plan-9", previewEvent.Preview.RenderPlanId);
        Assert.Equal("d3d11", previewEvent.Preview.Renderer);
        Assert.Equal(pixels, previewEvent.Preview.Bgra);
        Assert.Null(CoreProtocolParser.TryParseProgramFramePreviewEvent("""{"id":"1","type":"program-frame-preview"}"""));
    }

    [Theory]
    [InlineData("0xFEEDFACE", 0xFEEDFACEul)]
    [InlineData("0xfeedface", 0xFEEDFACEul)]
    [InlineData("FEEDFACE", 0xFEEDFACEul)]
    [InlineData("0xABCD", 0xABCDul)]
    [InlineData("  0x10  ", 0x10ul)]
    [InlineData("0x00000000DEADBEEF", 0x00000000DEADBEEFul)]
    [InlineData("0000000042AABBCC", 0x0000000042AABBCCul)]
    public void ParseSharedHandleHexAcceptsCommonWireFormats(string handleHex, ulong expected)
    {
        Assert.Equal(expected, CoreProtocolParser.ParseSharedHandleHex(handleHex));
    }

    [Theory]
    [InlineData("0xFEEDFACE", false)]
    [InlineData("0x00000000DEADBEEF", true)]
    [InlineData("0x10", true)]
    public void ParseSharedHandleHexPresentabilityMatchesInteropRules(string handleHex, bool expectedPresentable)
    {
        var handle = CoreProtocolParser.ParseSharedHandleHex(handleHex);
        Assert.Equal(expectedPresentable, SharedTextureInteropRules.IsPresentableHandle(handle));
    }

    [Theory]
    [InlineData("")]
    [InlineData("   ")]
    [InlineData("0x")]
    [InlineData("not-hex")]
    [InlineData("0xZZZZ")]
    public void ParseSharedHandleHexRejectsInvalidValues(string handleHex)
    {
        Assert.Equal(0ul, CoreProtocolParser.ParseSharedHandleHex(handleHex));
    }

    [Fact]
    public void ParsesProgramSharedTextureEvent()
    {
        var eventLine = """
            {
              "type": "program-shared-texture",
              "texture": {
                "sharedHandleHex": "0xFEEDFACE",
                "width": 1920,
                "height": 1080,
                "format": "B8G8R8A8_UNORM",
                "frameNumber": 12
              }
            }
            """;

        var textureEvent = CoreProtocolParser.TryParseProgramSharedTextureEvent(eventLine);
        Assert.NotNull(textureEvent);
        Assert.Equal("0xFEEDFACE", textureEvent!.Texture.SharedHandleHex);
        Assert.Equal(1920, textureEvent.Texture.Width);
        Assert.Equal(1080, textureEvent.Texture.Height);
        Assert.Equal("B8G8R8A8_UNORM", textureEvent.Texture.Format);
        Assert.Equal(12, textureEvent.Texture.FrameNumber);
        Assert.Equal(0xFEEDFACEul, CoreProtocolParser.ParseSharedHandleHex(textureEvent.Texture.SharedHandleHex));
        Assert.False(SharedTextureInteropRules.IsPresentable(textureEvent.Texture));
    }

    [Fact]
    public void ParsesSharedTextureEmbeddedInProgramFramePreview()
    {
        var pixels = new byte[4];
        var base64 = Convert.ToBase64String(pixels);
        var eventLine = $$"""
            {
              "type": "program-frame-preview",
              "preview": {
                "frameNumber": 3,
                "width": 1,
                "height": 1,
                "renderer": "d3d11",
                "health": "live",
                "pixelFormat": "bgra",
                "bgraBase64": "{{base64}}",
                "sharedTexture": {
                  "sharedHandleHex": "0xABCD",
                  "width": 640,
                  "height": 360,
                  "format": "B8G8R8A8_UNORM",
                  "frameNumber": 3
                }
              }
            }
            """;

        var previewEvent = CoreProtocolParser.TryParseProgramFramePreviewEvent(eventLine);
        Assert.NotNull(previewEvent);
        Assert.NotNull(previewEvent!.Preview.SharedTexture);
        Assert.Equal("0xABCD", previewEvent.Preview.SharedTexture!.SharedHandleHex);
        Assert.Equal(640, previewEvent.Preview.SharedTexture.Width);
    }

    [Fact]
    public void AudioRoutingMatrixJsonCarriesPerSendDetail()
    {
        // The core has always published per-send routing detail in the snapshot;
        // this pins the camelCase wire shape the shell now consumes (phase B1).
        const string json =
            """{"status":"live","routedSendCount":1,"routedSourceCount":1,"summary":"1 send.","sends":[{"sourceId":"16778240","busId":"stream","gainDb":-3}]}""";
        var options = new JsonSerializerOptions
        {
            PropertyNamingPolicy = JsonNamingPolicy.CamelCase,
            PropertyNameCaseInsensitive = true
        };

        var matrix = JsonSerializer.Deserialize<NativeMediaCoreAudioRoutingMatrix>(json, options);

        Assert.NotNull(matrix);
        var send = Assert.Single(matrix!.Sends);
        Assert.Equal("16778240", send.SourceId);
        Assert.Equal("stream", send.BusId);
        Assert.Equal(-3.0, send.GainDb, 1);
    }
}
