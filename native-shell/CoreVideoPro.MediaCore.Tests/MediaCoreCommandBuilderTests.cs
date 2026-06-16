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
            StreamDestinations = ["rtmp", "ndi"],
            Graphics =
            [
                new("brand-bug", "CoreVideo Pro", "top-right", Enabled: true)
            ]
        });

        Assert.Contains(commands, command => command.Type == "prepare-encoder-session");
        Assert.Contains(commands, command => command.Type == "start-encoder-session");

        var output = commands.Single(command => command.Type == "start-program-output");
        Assert.Equal(["recording", "rtmp", "ndi"], GetStringArray(output, "destinations"));
        Assert.Equal(["p1", "p2"], GetStringArray(output, "isoParticipantIds"));

        var targets = commands.Single(command => command.Type == "set-recording-targets");
        Assert.Equal("Recordings/CoreVideo Pro", GetString(targets, "targetFolder"));
        Assert.Equal("Q2_Product_Update", GetString(targets, "filenamePrefix"));

        var recording = commands.Single(command => command.Type == "start-recording-session");
        Assert.Equal("Q2_Product_Update-p1-p2", GetString(recording, "sessionId"));
        Assert.Contains(commands, command => command.Type == "set-overlay-asset");
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