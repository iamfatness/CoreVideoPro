using CoreVideoPro.MediaCore.Models;

namespace CoreVideoPro.MediaCore.Services;

/// <summary>
/// Builds <c>zoom-media-spine-sync</c> payloads for the native media core.
/// Mirrors <c>src/engine/zoomMediaSpineSync.ts</c> and the live validation harness.
/// </summary>
public static class ZoomMediaSpinePayloadBuilder
{
    // 8, matching the product's advertised maxParticipantFeeds. This was 6, which
    // silently left the 7th and 8th camera-on participants with NO raw video
    // subscription at all — they showed as frozen/placeholder tiles with no error
    // anywhere (live meeting, 2026-08-09: seven cameras on, Susan Cho never
    // subscribed). If the SDK refuses the extra subscriptions, the engine's
    // per-participant downgrade ladder handles it LOUDLY (video_subscribe code +
    // video_resolution_downgraded), so the cap must not pre-censor what the SDK
    // might grant.
    public const int DefaultMaxVideoSubscriptions = 8;

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
        /// <summary>
        /// Operator opted in to raw capture (Studio "Engine On" toggle). The native
        /// engine only starts raw recording / requests recording rights when true,
        /// so it no longer fires automatically on meeting join.
        /// </summary>
        public bool StartCapture { get; init; }
        public IReadOnlyList<MediaCoreSceneRouteWire> ProgramSceneRoutes { get; init; } = [];
        public IReadOnlyList<MediaCoreSceneRouteWire> PreviewSceneRoutes { get; init; } = [];

        /// <summary>
        /// Ordered Show Input roster that drives the core-composited GPU multiview. Delivered on
        /// this frequent, reliable channel (the production sync only carries it on scene publishes,
        /// which almost never re-run). Null leaves the multiview untouched.
        /// </summary>
        public MediaCoreMultiviewLayout? Multiview { get; init; }
    }

    public static Dictionary<string, object?> Build(BuildInput input)
    {
        var participants = FilterParticipants(input.Participants, input.SelectedBreakoutRoomId)
            .Select(MapParticipant)
            .ToList();
        var subscriptions = BuildSubscriptions(
            participants,
            input.MaxVideoSubscriptions,
            input.ProgramSceneRoutes,
            input.PreviewSceneRoutes);
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
            ["startCapture"] = input.StartCapture,
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

        if (input.Multiview is { } multiview)
        {
            payload["multiview"] = BuildMultiviewPayload(multiview);
        }

        return payload;
    }

    public static Dictionary<string, object?> BuildFromProductionContext(
        MediaCoreProductionSyncContext context,
        BuildInput options)
    {
        return Build(options with { Participants = context.Participants, Recording = context.Recording });
    }

    /// <summary>
    /// Serializes the multiview layout into the same per-source shape the standalone
    /// <c>set-multiview-layout</c> command uses, so the core can parse both with one helper.
    /// </summary>
    private static Dictionary<string, object?> BuildMultiviewPayload(MediaCoreMultiviewLayout multiview) =>
        new()
        {
            ["canvasWidth"] = multiview.CanvasWidth,
            ["canvasHeight"] = multiview.CanvasHeight,
            ["cols"] = multiview.Cols,
            ["rows"] = multiview.Rows,
            ["sources"] = multiview.Sources.Select(source => new Dictionary<string, object?>
            {
                ["sourceId"] = source.SourceId,
                ["kind"] = source.Kind,
                ["participantId"] = source.ParticipantId,
                ["captureDeviceId"] = source.CaptureDeviceId,
                ["mediaAssetId"] = source.MediaAssetId,
                ["slot"] = source.Slot,
                ["label"] = source.Label
            }).ToList()
        };

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
        int maxVideoSubscriptions,
        IReadOnlyList<MediaCoreSceneRouteWire> programSceneRoutes,
        IReadOnlyList<MediaCoreSceneRouteWire> previewSceneRoutes)
    {
        var subscriptions = new List<Dictionary<string, object?>>();
        if (participants.Count > 0)
        {
            // The meeting mix is an audio source in its own right. Do not make
            // its lifetime depend on an active-speaker video subscription.
            subscriptions.Add(new Dictionary<string, object?>
            {
                ["participantId"] = participants[0]["sdkUserId"]?.ToString() ?? string.Empty,
                ["kind"] = "meeting-audio",
                ["purpose"] = "program",
                ["priority"] = 0
            });
        }
        // Spend the finite raw-video budget in show order: directed speaker,
        // Program, queued Preview, then multiview-only roster feeds. This makes
        // a Take warm even when its participant sits beyond the roster cap.
        var participantIds = participants
            .Select(participant => participant["sdkUserId"]?.ToString() ?? string.Empty)
            .Where(participantId => participantId.Length > 0)
            .ToHashSet(StringComparer.Ordinal);
        var videoCandidates = new List<(string ParticipantId, string Purpose)>();
        var seenVideoParticipants = new HashSet<string>(StringComparer.Ordinal);

        void AddVideoCandidate(string? participantId, string purpose)
        {
            if (string.IsNullOrWhiteSpace(participantId) ||
                !participantIds.Contains(participantId) ||
                !seenVideoParticipants.Add(participantId))
            {
                return;
            }

            videoCandidates.Add((participantId, purpose));
        }

        var activeSpeaker = participants.FirstOrDefault(participant =>
            participant.TryGetValue("talking", out var talking) && talking is true);
        AddVideoCandidate(activeSpeaker?["sdkUserId"]?.ToString(), "active-speaker");

        foreach (var route in programSceneRoutes)
        {
            AddVideoCandidate(route.ParticipantId, "program");
        }

        foreach (var route in previewSceneRoutes)
        {
            AddVideoCandidate(route.ParticipantId, "preview");
        }

        foreach (var participant in participants)
        {
            AddVideoCandidate(participant["sdkUserId"]?.ToString(), "multiview");
        }

        var videoCount = Math.Min(Math.Max(0, maxVideoSubscriptions), videoCandidates.Count);
        for (var index = 0; index < videoCount; index++)
        {
            var candidate = videoCandidates[index];
            subscriptions.Add(new Dictionary<string, object?>
            {
                ["participantId"] = candidate.ParticipantId,
                ["kind"] = "participant-video",
                ["purpose"] = candidate.Purpose,
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
