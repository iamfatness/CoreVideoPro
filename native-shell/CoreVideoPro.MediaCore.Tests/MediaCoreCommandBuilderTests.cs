using System.Text.Json;
using CoreVideoPro.MediaCore.Models;
using CoreVideoPro.MediaCore.Services;
using Xunit;

namespace CoreVideoPro.MediaCore.Tests;

public sealed class MediaCoreCommandBuilderTests
{
    private static readonly IReadOnlyList<MediaCoreParticipantWire> Participants =
    [
        new(
            Id: "p1",
            Name: "Sophia Martinez",
            Role: "Host",
            BreakoutRoomId: "main",
            BreakoutRoomName: "Main room",
            IsActiveSpeaker: false,
            IsMuted: false,
            IsScreenSharing: false,
            AudioLevel: 54,
            Health: "live"),
        new(
            Id: "p2",
            Name: "David Chen",
            Role: "Presenter",
            BreakoutRoomId: "main",
            BreakoutRoomName: "Main room",
            IsActiveSpeaker: true,
            IsMuted: false,
            IsScreenSharing: true,
            AudioLevel: 82,
            Health: "live")
    ];

    [Fact]
    public void SerializesActiveSceneRoutesForSceneGraph()
    {
        var commands = MediaCoreCommandBuilder.BuildSyncCommands(new MediaCoreProductionSyncContext
        {
            ActiveSceneId = "interview",
            SceneRoutes =
            [
                new("interview-1", "active-speaker", "mix", null),
                new("interview-2", "fixed", "isolated", "p1")
            ],
            Participants = Participants
        });

        var sceneGraph = commands.Single(command => command.Type == "load-scene-graph");
        var sceneId = GetString(sceneGraph, "sceneId");
        var routes = GetRoutes(sceneGraph);

        Assert.Equal("interview", sceneId);
        Assert.Collection(
            routes,
            route =>
            {
                Assert.Equal("interview-1", route.RouteId);
                Assert.Equal("active-speaker", route.Mode);
                Assert.Equal("mix", route.AudioRole);
                Assert.Null(route.ParticipantId);
            },
            route =>
            {
                Assert.Equal("interview-2", route.RouteId);
                Assert.Equal("fixed", route.Mode);
                Assert.Equal("isolated", route.AudioRole);
                Assert.Equal("p1", route.ParticipantId);
            });
    }

    [Fact]
    public void SerializesCanvasRectsForSceneGraphRoutes()
    {
        var commands = MediaCoreCommandBuilder.BuildSyncCommands(new MediaCoreProductionSyncContext
        {
            ActiveSceneId = "custom-canvas",
            SceneRoutes =
            [
                new("canvas-1", "fixed", "mix", "p1", 0.05, 0.1, 0.4, 0.45, 0),
                new("canvas-2", "active-speaker", "mix", null, 0.55, 0.2, 0.35, 0.6, 1)
            ],
            Participants = Participants
        });

        var sceneGraph = commands.Single(command => command.Type == "load-scene-graph");
        Assert.NotNull(sceneGraph.ExtensionData);
        var routes = sceneGraph.ExtensionData!["routes"].EnumerateArray().ToList();

        Assert.Equal(0.05, routes[0].GetProperty("rect").GetProperty("x").GetDouble());
        Assert.Equal(0.1, routes[0].GetProperty("rect").GetProperty("y").GetDouble());
        Assert.Equal(0.4, routes[0].GetProperty("rect").GetProperty("width").GetDouble());
        Assert.Equal(0.45, routes[0].GetProperty("rect").GetProperty("height").GetDouble());
        Assert.Equal(1, routes[1].GetProperty("zIndex").GetInt32());
    }

    [Fact]
    public void SerializesPerRouteColorGradeForSceneGraphRoutes()
    {
        var commands = MediaCoreCommandBuilder.BuildSyncCommands(new MediaCoreProductionSyncContext
        {
            ActiveSceneId = "graded-scene",
            SceneRoutes =
            [
                new(
                    "graded-1",
                    "fixed",
                    "mix",
                    "p1",
                    ColorGrade: new MediaCoreColorGradeWire("warm-film", 8, 12, -6, 15))
            ],
            Participants = Participants
        });

        var sceneGraph = commands.Single(command => command.Type == "load-scene-graph");
        var route = sceneGraph.ExtensionData!["routes"].EnumerateArray().Single();
        var grade = route.GetProperty("colorGrade");

        Assert.Equal("warm-film", grade.GetProperty("lut").GetString());
        Assert.Equal(8, grade.GetProperty("exposure").GetInt32());
        Assert.Equal(12, grade.GetProperty("contrast").GetInt32());
        Assert.Equal(-6, grade.GetProperty("saturation").GetInt32());
        Assert.Equal(15, grade.GetProperty("temperature").GetInt32());
    }

    [Fact]
    public void BuildsSpeakerSlidesRoutesFromPreviewSlotEditors()
    {
        var commands = MediaCoreCommandBuilder.BuildSyncCommands(new MediaCoreProductionSyncContext
        {
            ActiveSceneId = "speaker-slides",
            SceneRoutes =
            [
                new("speaker-slides-1", "fixed", "isolated", "p2"),
                new("speaker-slides-2", "screen-share", "audience", null)
            ],
            Participants = Participants
        });

        var routes = GetRoutes(commands.Single(command => command.Type == "load-scene-graph"));

        Assert.Collection(
            routes,
            route =>
            {
                Assert.Equal("fixed", route.Mode);
                Assert.Equal("isolated", route.AudioRole);
                Assert.Equal("p2", route.ParticipantId);
            },
            route =>
            {
                Assert.Equal("screen-share", route.Mode);
                Assert.Equal("audience", route.AudioRole);
                Assert.Null(route.ParticipantId);
            });
    }

    [Fact]
    public void ArmsRecordingAndStreamingOutputsWhenEnabled()
    {
        var commands = MediaCoreCommandBuilder.BuildSyncCommands(new MediaCoreProductionSyncContext
        {
            ActiveSceneId = "speaker-slides",
            SceneRoutes = [new("speaker-slides-1", "fixed", "isolated", "p2")],
            Participants = Participants,
            Recording = true,
            Streaming = true,
            CanvasOutputProfile = new MediaCoreOutputProfileWire("canvas-4k60", "3840x2160", 3840, 2160, 60, 28),
            StreamOutputProfile = new MediaCoreOutputProfileWire("stream-1080p30", "1920x1080", 1920, 1080, 30, 4.1),
            RecordingOutputProfile = new MediaCoreOutputProfileWire("recording-4k60", "3840x2160", 3840, 2160, 60, 28),
            StreamDestinations = ["rtmp", "ndi", "srt"],
            StreamDestinationSettings =
            [
                new("rtmp", "RTMP", Protocol: "rtmps", Url: "rtmps://live.example.com/app", StreamKey: "abc123"),
                new("ndi", "NDI", NdiName: "CoreVideo Pro Program", NdiGroup: "public"),
                new(
                    "srt",
                    "SRT",
                    Mode: "caller",
                    Host: "receiver.example.com",
                    Port: 9000,
                    LatencyMs: 120,
                    LatencyUs: 120000,
                    Passphrase: "secret-passphrase",
                    KeyLength: 32,
                    StreamId: "publish/live/main")
            ]
        });

        Assert.Contains(commands, command => command.Type == "prepare-encoder-session");
        Assert.Contains(commands, command => command.Type == "start-encoder-session");

        var outputProfile = commands.Single(command => command.Type == "set-output-profile");
        Assert.Equal("canvas-4k60", GetString(outputProfile, "profileId"));
        Assert.Equal("3840x2160", GetString(outputProfile, "resolution"));
        Assert.Equal(3840, outputProfile.ExtensionData!["width"].GetInt32());
        Assert.Equal(2160, outputProfile.ExtensionData!["height"].GetInt32());
        Assert.Equal(60, outputProfile.ExtensionData!["fps"].GetInt32());

        var output = commands.Single(command => command.Type == "start-program-output");
        Assert.Equal(["recording", "rtmp", "ndi", "srt"], GetStringArray(output, "destinations"));
        Assert.Empty(GetStringArray(output, "isoParticipantIds"));
        Assert.Equal("stream-1080p30", GetObject(output, "streamOutputProfile").GetProperty("profileId").GetString());
        Assert.Equal(30, GetObject(output, "streamOutputProfile").GetProperty("fps").GetInt32());
        Assert.Equal("recording-4k60", GetObject(output, "recordingOutputProfile").GetProperty("profileId").GetString());
        var destinationSettings = GetObjectArray(output, "destinationSettings");
        Assert.Equal(["rtmp", "ndi", "srt"], destinationSettings.Select(destination => destination.GetProperty("id").GetString()));
        Assert.Equal("rtmps", destinationSettings[0].GetProperty("protocol").GetString());
        Assert.Equal("rtmps://live.example.com/app", destinationSettings[0].GetProperty("url").GetString());
        Assert.Equal("abc123", destinationSettings[0].GetProperty("streamKey").GetString());
        Assert.Equal("CoreVideo Pro Program", destinationSettings[1].GetProperty("ndiName").GetString());
        Assert.Equal("public", destinationSettings[1].GetProperty("ndiGroup").GetString());
        Assert.Equal("caller", destinationSettings[2].GetProperty("mode").GetString());
        Assert.Equal("receiver.example.com", destinationSettings[2].GetProperty("host").GetString());
        Assert.Equal(9000, destinationSettings[2].GetProperty("port").GetInt32());
        Assert.Equal(120, destinationSettings[2].GetProperty("latencyMs").GetInt32());
        Assert.Equal(120000, destinationSettings[2].GetProperty("latencyUs").GetInt32());
        Assert.Equal("secret-passphrase", destinationSettings[2].GetProperty("passphrase").GetString());
        Assert.Equal(32, destinationSettings[2].GetProperty("keyLength").GetInt32());
        Assert.Equal("publish/live/main", destinationSettings[2].GetProperty("streamId").GetString());

        var targets = commands.Single(command => command.Type == "set-recording-targets");
        Assert.Equal("Recordings/CoreVideo Pro", GetString(targets, "targetFolder"));
        Assert.Equal("corevideo-recording", GetString(targets, "filenamePrefix"));
        Assert.Equal("recording-4k60", GetObject(targets, "renderProfile").GetProperty("profileId").GetString());

        var recording = commands.Single(command => command.Type == "start-recording-session");
        Assert.Equal("corevideo-recording-program", GetString(recording, "sessionId"));
        Assert.Equal("3840x2160", GetObject(recording, "renderProfile").GetProperty("resolution").GetString());
        Assert.DoesNotContain(commands, command => command.Type == "set-overlay-asset");
    }

    [Fact]
    public void StopsEncoderAndRecordingWhenOutputsAreIdle()
    {
        var commands = MediaCoreCommandBuilder.BuildSyncCommands(new MediaCoreProductionSyncContext
        {
            ActiveSceneId = "speaker-slides",
            SceneRoutes = [new("speaker-slides-1", "active-speaker", "mix", null)],
            Participants = Participants,
            Recording = false,
            Streaming = false
        });

        Assert.Contains(commands, command => command.Type == "stop-encoder-session");
        Assert.DoesNotContain(commands, command => command.Type == "start-program-output");
        Assert.DoesNotContain(commands, command => command.Type == "start-recording-session");

        var stopRecording = commands.Single(command => command.Type == "stop-recording-session");
        Assert.Equal(
            "Recording disabled in production state.",
            GetString(stopRecording, "reason"));
    }

    [Fact]
    public void BuildsFullTakeSyncBatchWithSceneRoutesRosterAndOutputs()
    {
        var commands = MediaCoreCommandBuilder.BuildSyncCommands(new MediaCoreProductionSyncContext
        {
            ActiveSceneId = "speaker-slides",
            SceneRoutes =
            [
                new("speaker-slides-1", "fixed", "isolated", "p2"),
                new("speaker-slides-2", "screen-share", "audience", null)
            ],
            Participants = Participants,
            Recording = true,
            Streaming = true,
            StreamDestinations = ["rtmp", "ndi"],
            CaptionText = "Welcome to the show.",
            CaptionSpeaker = "Sophia Martinez"
        });

        var sceneGraph = commands.Single(command => command.Type == "load-scene-graph");
        Assert.Equal("speaker-slides", GetString(sceneGraph, "sceneId"));
        Assert.Equal(2, GetRoutes(sceneGraph).Count);

        var commandTypes = commands.Select(command => command.Type).ToList();
        Assert.Contains("set-zoom-source-roster", commandTypes);
        Assert.Contains("set-active-speaker", commandTypes);
        Assert.Contains("set-screen-share-source", commandTypes);
        Assert.Contains("load-scene-graph", commandTypes);
        Assert.Contains("prepare-encoder-session", commandTypes);
        Assert.Contains("start-program-output", commandTypes);
        Assert.Contains("start-encoder-session", commandTypes);
        Assert.Contains("set-recording-targets", commandTypes);
        Assert.Contains("start-recording-session", commandTypes);
        Assert.Contains("push-caption-cue", commandTypes);
    }

    [Fact]
    public void ArmsRecordingOnlyWithoutStreamingDestinations()
    {
        var commands = MediaCoreCommandBuilder.BuildSyncCommands(new MediaCoreProductionSyncContext
        {
            ActiveSceneId = "interview",
            SceneRoutes = [new("interview-1", "active-speaker", "mix", null)],
            Participants = Participants,
            Recording = true,
            Streaming = false
        });

        var output = commands.Single(command => command.Type == "start-program-output");
        Assert.Equal(["recording"], GetStringArray(output, "destinations"));
        Assert.Contains(commands, command => command.Type == "start-recording-session");
        Assert.DoesNotContain(commands, command => command.Type == "stop-recording-session");
    }

    [Fact]
    public void ArmsStreamingOnlyWithoutRecordingSession()
    {
        var commands = MediaCoreCommandBuilder.BuildSyncCommands(new MediaCoreProductionSyncContext
        {
            ActiveSceneId = "interview",
            SceneRoutes = [new("interview-1", "active-speaker", "mix", null)],
            Participants = Participants,
            Recording = false,
            Streaming = true,
            StreamDestinations = ["rtmp"]
        });

        var output = commands.Single(command => command.Type == "start-program-output");
        Assert.Equal(["rtmp"], GetStringArray(output, "destinations"));
        Assert.DoesNotContain(commands, command => command.Type == "start-recording-session");

        var stopRecording = commands.Single(command => command.Type == "stop-recording-session");
        Assert.Equal("Recording disabled in production state.", GetString(stopRecording, "reason"));
    }

    [Fact]
    public void IncludesZoomRosterActiveSpeakerAndScreenShareCommands()
    {
        var commands = MediaCoreCommandBuilder.BuildSyncCommands(new MediaCoreProductionSyncContext
        {
            ActiveSceneId = "speaker-slides",
            SceneRoutes = [new("speaker-slides-1", "fixed", "isolated", "p2")],
            Participants = Participants
        });

        var roster = commands.Single(command => command.Type == "set-zoom-source-roster");
        Assert.Contains(
            GetSourceParticipantIds(roster),
            id => id == "p2");

        var activeSpeaker = commands.Single(command => command.Type == "set-active-speaker");
        Assert.Equal("p2", GetString(activeSpeaker, "participantId"));

        var screenShare = commands.Single(command => command.Type == "set-screen-share-source");
        Assert.Equal("p2", GetString(screenShare, "participantId"));
    }

    [Fact]
    public void SerializesAudioRoutingGainMatrixSends()
    {
        var commands = MediaCoreCommandBuilder.BuildSyncCommands(new MediaCoreProductionSyncContext
        {
            ActiveSceneId = "speaker-slides",
            SceneRoutes = [new("speaker-slides-1", "fixed", "isolated", "p2")],
            Participants = Participants,
            AudioRoutingSends =
            [
                new("input-01", "pgm-l", -3.0),
                new("input-01", "pgm-r", -3.0),
                new("input-01", "mon", -6.0),
                new("input-02", "pgm-l", 0.0)
            ]
        });

        var routing = commands.Single(command => command.Type == "sync-audio-routing-matrix");
        Assert.NotNull(routing.ExtensionData);
        var sends = routing.ExtensionData!["sends"].EnumerateArray().ToList();

        Assert.Equal(4, sends.Count);
        Assert.Equal("input-01", sends[0].GetProperty("sourceId").GetString());
        Assert.Equal("pgm-l", sends[0].GetProperty("busId").GetString());
        Assert.Equal(-3.0, sends[0].GetProperty("gainDb").GetDouble());
        Assert.Equal("mon", sends[2].GetProperty("busId").GetString());
        Assert.Equal(-6.0, sends[2].GetProperty("gainDb").GetDouble());
    }

    [Fact]
    public void EmitsEmptyAudioRoutingMatrixWhenNoSendsAreRouted()
    {
        var commands = MediaCoreCommandBuilder.BuildSyncCommands(new MediaCoreProductionSyncContext
        {
            ActiveSceneId = "interview",
            SceneRoutes = [new("interview-1", "active-speaker", "mix", null)],
            Participants = Participants
        });

        var routing = commands.Single(command => command.Type == "sync-audio-routing-matrix");
        Assert.NotNull(routing.ExtensionData);
        Assert.Empty(routing.ExtensionData!["sends"].EnumerateArray());
    }

    [Fact]
    public void PushesMediaPlaybackCommandOnlyWhenAnAssetIsSelected()
    {
        var withoutSelection = MediaCoreCommandBuilder.BuildSyncCommands(new MediaCoreProductionSyncContext
        {
            ActiveSceneId = "interview",
            SceneRoutes = [new("interview-1", "active-speaker", "mix", null)],
            Participants = Participants
        });

        Assert.DoesNotContain(withoutSelection, command => command.Type == "set-media-playback");

        var withSelection = MediaCoreCommandBuilder.BuildSyncCommands(new MediaCoreProductionSyncContext
        {
            ActiveSceneId = "interview",
            SceneRoutes = [new("interview-1", "active-speaker", "mix", null)],
            Participants = Participants,
            SelectedMediaAssetId = "clip-intro",
            SelectedMediaAssetName = "Intro Sting",
            SelectedMediaAssetKind = "stinger",
            SelectedMediaAssetPath = @"C:\media\intro.mp4",
            SelectedMediaAssetPlaying = true
        });

        var playback = withSelection.Single(command => command.Type == "set-media-playback");
        Assert.Equal("clip-intro", GetString(playback, "mediaAssetId"));
        Assert.Equal("Intro Sting", GetString(playback, "mediaAssetName"));
        Assert.Equal("stinger", GetString(playback, "mediaAssetKind"));
        Assert.Equal(@"C:\media\intro.mp4", GetString(playback, "mediaAssetPath"));
        Assert.NotNull(playback.ExtensionData);
        Assert.True(playback.ExtensionData!["playing"].GetBoolean());
    }

    [Fact]
    public void PushesConcreteBrandKitFieldsToNativeCore()
    {
        var commands = MediaCoreCommandBuilder.BuildSyncCommands(new MediaCoreProductionSyncContext
        {
            ActiveSceneId = "interview",
            SceneRoutes = [new("interview-1", "active-speaker", "mix", null)],
            Participants = Participants,
            BrandKit = new MediaCoreBrandKitWire(
                "Launch Briefing",
                "CVP",
                "logo-main",
                "Primary Logo",
                @"C:\media\brand\logo.png",
                "#1cc7b8",
                "#f2b84b",
                "#081014",
                "Inter",
                "executive",
                "boxed",
                "auto")
        });

        var brand = commands.Single(command => command.Type == "set-brand-kit");
        Assert.Equal("Launch Briefing", GetString(brand, "name"));
        Assert.Equal("CVP", GetString(brand, "logoText"));
        Assert.Equal("logo-main", GetString(brand, "logoAssetId"));
        Assert.Equal("Primary Logo", GetString(brand, "logoAssetName"));
        Assert.Equal(@"C:\media\brand\logo.png", GetString(brand, "logoAssetPath"));
        Assert.Equal("#1cc7b8", GetString(brand, "brandColor"));
        Assert.Equal("#f2b84b", GetString(brand, "accentColor"));
        Assert.Equal("#081014", GetString(brand, "backgroundColor"));
        Assert.Equal("Inter", GetString(brand, "fontFamily"));
        Assert.Equal("executive", GetString(brand, "lowerThirdStyle"));
        Assert.Equal("boxed", GetString(brand, "captionStyle"));
        Assert.Equal("auto", GetString(brand, "defaultOverlayBehavior"));
    }

    private static string? GetString(NativeMediaCoreCommand command, string propertyName)
    {
        if (command.ExtensionData is null ||
            !command.ExtensionData.TryGetValue(propertyName, out var value) ||
            value.ValueKind != JsonValueKind.String)
        {
            return null;
        }

        return value.GetString();
    }

    private static IReadOnlyList<string> GetStringArray(NativeMediaCoreCommand command, string propertyName)
    {
        if (command.ExtensionData is null ||
            !command.ExtensionData.TryGetValue(propertyName, out var value) ||
            value.ValueKind != JsonValueKind.Array)
        {
            return [];
        }

        return value.EnumerateArray()
            .Where(element => element.ValueKind == JsonValueKind.String)
            .Select(element => element.GetString()!)
            .ToList();
    }

    private static IReadOnlyList<JsonElement> GetObjectArray(NativeMediaCoreCommand command, string propertyName)
    {
        if (command.ExtensionData is null ||
            !command.ExtensionData.TryGetValue(propertyName, out var value) ||
            value.ValueKind != JsonValueKind.Array)
        {
            return [];
        }

        return value.EnumerateArray()
            .Where(element => element.ValueKind == JsonValueKind.Object)
            .ToList();
    }

    private static JsonElement GetObject(NativeMediaCoreCommand command, string propertyName)
    {
        Assert.NotNull(command.ExtensionData);
        Assert.True(command.ExtensionData!.TryGetValue(propertyName, out var value));
        Assert.Equal(JsonValueKind.Object, value.ValueKind);
        return value;
    }

    private static IReadOnlyList<(string RouteId, string Mode, string AudioRole, string? ParticipantId)> GetRoutes(
        NativeMediaCoreCommand command)
    {
        if (command.ExtensionData is null ||
            !command.ExtensionData.TryGetValue("routes", out var routesElement) ||
            routesElement.ValueKind != JsonValueKind.Array)
        {
            return [];
        }

        return routesElement.EnumerateArray()
            .Select(route =>
            {
                var routeId = route.TryGetProperty("routeId", out var routeIdElement)
                    ? routeIdElement.GetString() ?? string.Empty
                    : string.Empty;
                var mode = route.TryGetProperty("mode", out var modeElement)
                    ? modeElement.GetString() ?? string.Empty
                    : string.Empty;
                var audioRole = route.TryGetProperty("audioRole", out var audioRoleElement)
                    ? audioRoleElement.GetString() ?? string.Empty
                    : string.Empty;
                string? participantId = route.TryGetProperty("participantId", out var participantElement) &&
                                          participantElement.ValueKind == JsonValueKind.String
                    ? participantElement.GetString()
                    : null;
                return (routeId, mode, audioRole, participantId);
            })
            .ToList();
    }

    private static IReadOnlyList<string> GetSourceParticipantIds(NativeMediaCoreCommand command)
    {
        if (command.ExtensionData is null ||
            !command.ExtensionData.TryGetValue("sources", out var sourcesElement) ||
            sourcesElement.ValueKind != JsonValueKind.Array)
        {
            return [];
        }

        return sourcesElement.EnumerateArray()
            .Select(source => source.TryGetProperty("participantId", out var participantElement) &&
                              participantElement.ValueKind == JsonValueKind.String
                ? participantElement.GetString()!
                : string.Empty)
            .Where(id => id.Length > 0)
            .ToList();
    }
}
