using CoreVideoPro.MediaCore.Models;

namespace CoreVideoPro.MediaCore.Services;

/// <summary>
/// Builds <c>zoom-media-spine-sync</c> payloads for the native media core.
/// Mirrors <c>src/engine/zoomMediaSpineSync.ts</c> and the live validation harness.
/// </summary>
public static class ZoomMediaSpinePayloadBuilder
{
    // Ten matches the Show Input and Multiview capacity contract. This was 6, then 8, which
    // silently left the 7th and 8th camera-on participants with NO raw video
    // subscription at all — they showed as frozen/placeholder tiles with no error
    // anywhere (live meeting, 2026-08-09: seven cameras on, Susan Cho never
    // subscribed). If the SDK refuses the extra subscriptions, the engine's
    // per-participant downgrade ladder handles it LOUDLY (video_subscribe code +
    // video_resolution_downgraded), so the cap must not pre-censor what the SDK
    // might grant.
    public const int DefaultMaxVideoSubscriptions = 10;

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
        /// The Tiles wall of the PROGRAM scene, or null when Program is not a Tiles scene. A
        /// Tiles scene serialises an EMPTY route list, so without this its members would be
        /// invisible to the video budget.
        /// </summary>
        public MediaCoreTilesLayerWire? ProgramTilesLayer { get; init; }

        /// <summary>The Tiles wall of the PREVIEW scene, or null.</summary>
        public MediaCoreTilesLayerWire? PreviewTilesLayer { get; init; }

        /// <summary>
        /// Bare Zoom participant ids armed for ISO recording. Empty unless "Program + ISOs" is
        /// on (<see cref="IsoSourceSelectionResolver.Resolve"/> returns nothing when disabled).
        /// An ISO guest needs video even when off the wall, or the ISO file has no picture.
        /// </summary>
        public IReadOnlyList<string> IsoParticipantIds { get; init; } = [];

        /// <summary>
        /// Bare Zoom ids latched by <see cref="TilesAudioSourceLatch"/>: Tiles members of a scene
        /// still on its bus, kept as AUDIO sources after their camera goes off (#478 R3).
        /// </summary>
        public IReadOnlyList<string> StickyAudioParticipantIds { get; init; } = [];

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
        // ONE source set feeds both the video and the audio lists (#478 owner rule: only
        // sources that are sources; nothing is on air unless it is an input).
        var sources = ZoomSourceSetPolicy.Resolve(new ZoomSourceSetPolicy.Input
        {
            Participants = participants
                .Select(participant => new ZoomSourceSetPolicy.ParticipantState(
                    participant["sdkUserId"]?.ToString() ?? string.Empty,
                    participant["videoOn"] is true,
                    participant["talking"] is true))
                .ToList(),
            ProgramRoutes = input.ProgramSceneRoutes,
            ProgramTiles = input.ProgramTilesLayer,
            PreviewRoutes = input.PreviewSceneRoutes,
            PreviewTiles = input.PreviewTilesLayer,
            StickyAudioParticipantIds = input.StickyAudioParticipantIds,
            WallSources = input.Multiview?.Sources ?? [],
            IsoParticipantIds = input.IsoParticipantIds
        });
        var videoDecision = ZoomSourceSetPolicy.DecideVideo(sources, input.MaxVideoSubscriptions);
        var subscriptions = BuildSubscriptions(
            participants,
            videoDecision.Subscribed,
            ZoomSourceSetPolicy.AudioParticipantIds(sources));
        var readiness = BuildReadiness(input);
        var warnings = new List<string>();
        if (videoDecision.OverBudget.Count > 0)
        {
            // Loud, never silent: a routed camera-on guest with no video is a black tile.
            warnings.Add(
                $"No video: subscription limit ({input.MaxVideoSubscriptions}). " +
                $"{videoDecision.OverBudget.Count} routed camera{(videoDecision.OverBudget.Count == 1 ? " is" : "s are")} unsubscribed: " +
                string.Join(", ", videoDecision.OverBudget.Select(candidate =>
                    $"{DisplayName(participants, candidate.ParticipantId)} ({candidate.Purpose})")) + ".");
        }
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
            ["summary"] = summary,
            // The core's speaker director follows the talker ONLY among these (#478 R1): a
            // non-source never has a subscription, so it could never pass the director's
            // fresh-frame gate and would deadlock speaker-following.
            ["sourceParticipantIds"] = ZoomSourceSetPolicy.SpeakerCandidateIds(sources).ToList(),
            // Structured twin of the warning above, for tests and the support bundle.
            ["videoSubscriptionShortfall"] = videoDecision.OverBudget
                .Select(candidate => new Dictionary<string, object?>
                {
                    ["participantId"] = candidate.ParticipantId,
                    ["purpose"] = candidate.Purpose,
                    ["reason"] = "subscription-limit"
                })
                .ToList()
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
            var unsubscribedWallGuests = videoDecision.OverBudget
                .Select(candidate => candidate.ParticipantId)
                .ToHashSet(StringComparer.Ordinal);
            payload["multiview"] = BuildMultiviewPayload(
                multiview,
                unsubscribedWallGuests,
                input.MaxVideoSubscriptions);
        }

        return payload;
    }

    /// <summary>
    /// The multiview overlay draws each tile's <c>label</c> (core <c>buildMultiviewTiles</c> →
    /// <c>MultiviewOverlayFormatting.ResolveLabel</c>), so a wall guest the budget left out says
    /// so ON the tile instead of sitting there as an unexplained empty box.
    /// </summary>
    public static string UnsubscribedTileLabel(string label, int maxVideoSubscriptions) =>
        $"{label} · no video: subscription limit ({maxVideoSubscriptions})";

    private static string DisplayName(IReadOnlyList<Dictionary<string, object?>> participants, string participantId) =>
        participants
            .FirstOrDefault(participant => participant["sdkUserId"]?.ToString() == participantId)?["displayName"]?.ToString()
        is { Length: > 0 } name
            ? name
            : participantId;

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
    private static Dictionary<string, object?> BuildMultiviewPayload(
        MediaCoreMultiviewLayout multiview,
        IReadOnlySet<string> unsubscribedParticipantIds,
        int maxVideoSubscriptions) =>
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
                ["label"] = string.Equals(source.Kind, "zoom", StringComparison.Ordinal) &&
                            source.ParticipantId is { Length: > 0 } participantId &&
                            unsubscribedParticipantIds.Contains(participantId)
                    ? UnsubscribedTileLabel(source.Label, maxVideoSubscriptions)
                    : source.Label
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
        IReadOnlyList<ZoomSourceSetPolicy.Source> video,
        IReadOnlyList<string> audioParticipantIds)
    {
        var subscriptions = new List<Dictionary<string, object?>>();
        if (participants.Count > 0)
        {
            // The meeting mix is an audio source in its own right. Do not make
            // its lifetime depend on an active-speaker video subscription.
            // Deliberately NOT narrowed to the source set: it is the programMix-mode
            // path (Zoom's own mix), and is left unrouted in perGuestIso mode.
            subscriptions.Add(new Dictionary<string, object?>
            {
                ["participantId"] = participants[0]["sdkUserId"]?.ToString() ?? string.Empty,
                ["kind"] = "meeting-audio",
                ["purpose"] = "program",
                ["priority"] = 0
            });
        }

        // WHO gets raw video, in what order and at what purpose is ZoomSourceSetPolicy's
        // decision (#478): camera-on sources only, capped, never by roster order.
        for (var index = 0; index < video.Count; index++)
        {
            var candidate = video[index];
            subscriptions.Add(new Dictionary<string, object?>
            {
                ["participantId"] = candidate.ParticipantId,
                ["kind"] = "participant-video",
                ["purpose"] = candidate.Purpose,
                ["priority"] = 10 + index
            });
        }

        // Isolated per-participant AUDIO for the mixer and the per-guest ISO stems: the SAME
        // source set as video (owner ruling, #478, option 1 "sources only"), except that a
        // camera-OFF source keeps its audio — a wall guest with the camera off can still speak.
        // Anyone who is not a source is not subscribed and is inaudible in Program: nothing is
        // on air unless it is an input.
        for (var index = 0; index < audioParticipantIds.Count; index++)
        {
            subscriptions.Add(new Dictionary<string, object?>
            {
                ["participantId"] = audioParticipantIds[index],
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
