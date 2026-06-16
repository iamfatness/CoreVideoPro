using CoreVideoPro.MediaCore.Models;

namespace CoreVideoPro.MediaCore.Services;

/// <summary>
/// Builds <c>zoom-media-spine-sync</c> payloads for the native media core.
/// Mirrors <c>src/engine/zoomMediaSpineSync.ts</c> and the live validation harness.
/// </summary>
public static class ZoomMediaSpinePayloadBuilder
{
    public const int DefaultMaxVideoSubscriptions = 6;

    public sealed record BuildInput
    {
        public IReadOnlyList<MediaCoreParticipantWire> Participants { get; init; } = [];
        public bool Recording { get; init; }
        public string SelectedBreakoutRoomId { get; init; } = "main";
        public int MaxVideoSubscriptions { get; init; } = DefaultMaxVideoSubscriptions;
        public string SdkVersion { get; init; } = "zoom-engine";
        public bool EngineRunning { get; init; }
        public bool OAuthSignedIn { get; init; }
        public bool SdkRuntimeReady { get; init; } = true;
    }

    public static Dictionary<string, object?> Build(BuildInput input)
    {
        var participants = FilterParticipants(input.Participants, input.SelectedBreakoutRoomId)
            .Select(MapParticipant)
            .ToList();
        var subscriptions = BuildSubscriptions(participants, input.MaxVideoSubscriptions);
        var readiness = BuildReadiness(input);
        var warnings = new List<string>();
        if (!input.EngineRunning)
        {
            warnings.Add("Media core is not running.");
        }

        if (!input.OAuthSignedIn)
        {
            warnings.Add("Zoom OAuth sign-in is recommended for external-account meetings.");
        }

        var blocked = !input.EngineRunning || !input.SdkRuntimeReady;
        var summary = blocked
            ? $"Zoom media spine sync blocked; {warnings.Count} warning{(warnings.Count == 1 ? "" : "s")} require attention."
            : $"{participants.Count} Zoom participant{(participants.Count == 1 ? "" : "s")}, {subscriptions.Count} raw subscriptions requested.";

        var payload = new Dictionary<string, object?>
        {
            ["readiness"] = readiness,
            ["participants"] = participants,
            ["subscriptions"] = subscriptions,
            ["blocked"] = blocked,
            ["warnings"] = warnings,
            ["summary"] = summary
        };

        if (input.Recording)
        {
            payload["recording"] = new Dictionary<string, object?>
            {
                ["targetFolder"] = MediaCoreProductionSyncContext.DefaultRecordingTargets.TargetFolder,
                ["filenamePrefix"] = MediaCoreProductionSyncContext.DefaultRecordingTargets.FilenamePrefix,
                ["format"] = MediaCoreProductionSyncContext.DefaultRecordingTargets.Format,
                ["quality"] = MediaCoreProductionSyncContext.DefaultRecordingTargets.Quality,
                ["isoParticipantIds"] = MediaCoreProductionSyncContext.DefaultRecordingTargets.IsoParticipantIds
            };
        }

        return payload;
    }

    public static Dictionary<string, object?> BuildFromProductionContext(
        MediaCoreProductionSyncContext context,
        BuildInput options)
    {
        return Build(options with { Participants = context.Participants, Recording = context.Recording });
    }

    private static IReadOnlyList<MediaCoreParticipantWire> FilterParticipants(
        IReadOnlyList<MediaCoreParticipantWire> participants,
        string selectedBreakoutRoomId)
    {
        if (selectedBreakoutRoomId.Equals("all", StringComparison.Ordinal))
        {
            return participants;
        }

        return participants
            .Where(participant => participant.BreakoutRoomId.Equals(selectedBreakoutRoomId, StringComparison.Ordinal))
            .ToList();
    }

    private static Dictionary<string, object?> MapParticipant(MediaCoreParticipantWire participant)
    {
        return new Dictionary<string, object?>
        {
            ["sdkUserId"] = participant.Id,
            ["displayName"] = participant.Name,
            ["role"] = MapRole(participant.Role),
            ["videoOn"] = participant.Health != "video-off",
            ["muted"] = participant.IsMuted,
            ["talking"] = participant.IsActiveSpeaker,
            ["sharingScreen"] = participant.IsScreenSharing,
            ["audioLevel"] = participant.AudioLevel,
            ["networkQuality"] = MapNetworkQuality(participant.Health)
        };
    }

    private static string MapRole(string role) => role.ToLowerInvariant() switch
    {
        "host" => "host",
        "presenter" or "panelist" => "panelist",
        _ => "guest"
    };

    private static string MapNetworkQuality(string health) => health switch
    {
        "low-resolution" => "low",
        "recovering" => "recovering",
        _ => "good"
    };

    private static List<Dictionary<string, object?>> BuildSubscriptions(
        IReadOnlyList<Dictionary<string, object?>> participants,
        int maxVideoSubscriptions)
    {
        var subscriptions = new List<Dictionary<string, object?>>();
        var videoCount = Math.Min(maxVideoSubscriptions, participants.Count);
        for (var index = 0; index < videoCount; index++)
        {
            var participant = participants[index];
            var participantId = participant["sdkUserId"]?.ToString() ?? string.Empty;
            subscriptions.Add(new Dictionary<string, object?>
            {
                ["participantId"] = participantId,
                ["kind"] = "participant-video",
                ["purpose"] = index == 0 ? "active-speaker" : "program",
                ["priority"] = 10 + index
            });
        }

        for (var index = 0; index < participants.Count; index++)
        {
            var participant = participants[index];
            subscriptions.Add(new Dictionary<string, object?>
            {
                ["participantId"] = participant["sdkUserId"]?.ToString() ?? string.Empty,
                ["kind"] = "participant-audio",
                ["purpose"] = "mix",
                ["priority"] = 40 + index
            });
        }

        return subscriptions;
    }

    private static Dictionary<string, object?> BuildReadiness(BuildInput input)
    {
        var checks = new List<Dictionary<string, object?>>
        {
            Check("sdk-runtime", input.SdkRuntimeReady, "Zoom Meeting SDK runtime",
                input.SdkRuntimeReady ? "Runtime available for spine sync." : "Zoom SDK runtime is not staged."),
            Check("oauth", input.OAuthSignedIn || input.EngineRunning, "OAuth broker",
                input.OAuthSignedIn ? "Signed in with Zoom." : "OAuth sign-in optional for dev joins."),
            Check("raw-video", input.EngineRunning, "Raw participant video",
                input.EngineRunning ? "Raw participant video requested." : "Engine off."),
            Check("raw-audio", input.EngineRunning, "Raw participant audio",
                input.EngineRunning ? "Raw participant audio requested." : "Engine off.")
        };

        var blockers = checks
            .Where(check => check["status"]?.ToString() == "blocked")
            .Select(check => check["detail"]?.ToString() ?? string.Empty)
            .Where(detail => detail.Length > 0)
            .ToList();

        return new Dictionary<string, object?>
        {
            ["status"] = blockers.Count > 0 ? "blocked" : "ready",
            ["platform"] = "windows",
            ["sdkVersion"] = input.SdkVersion,
            ["checks"] = checks,
            ["blockers"] = blockers,
            ["warnings"] = Array.Empty<string>(),
            ["summary"] = blockers.Count > 0
                ? "Zoom SDK media path blocked."
                : "Zoom SDK media path ready for spine sync."
        };
    }

    private static Dictionary<string, object?> Check(
        string id,
        bool ready,
        string label,
        string detail) =>
        new()
        {
            ["id"] = id,
            ["status"] = ready ? "ready" : "blocked",
            ["label"] = label,
            ["detail"] = detail
        };
}