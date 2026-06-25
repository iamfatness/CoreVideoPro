using CoreVideoPro.MediaCore.Models;

namespace CoreVideoPro.MediaCore.Services;

/// <summary>
/// Maps immutable media-core snapshots into studio live-production readouts.
/// Mirrors the React <c>liveProductionSync.ts</c> + snapshot readout wiring.
/// </summary>
public static class LiveProductionSync
{
    public sealed record LiveProductionParticipantContext
    {
        public required string Id { get; init; }
        public required string Name { get; init; }
        public string Title { get; init; } = string.Empty;
        public required string RoleLabel { get; init; }
        public string BreakoutRoomId { get; init; } = "main";
        public string BreakoutRoomName { get; init; } = string.Empty;
        public bool IsActiveSpeaker { get; init; }
        public bool IsScreenSharing { get; init; }
        public bool IsMuted { get; init; }
        public int AudioLevel { get; init; }
        public string HealthLabel { get; init; } = "live";
    }

    public sealed record LiveProductionSyncContext
    {
        public required string ActiveSceneId { get; init; }
        public required string ActiveSceneLayout { get; init; }
        public required string CurrentBreakoutRoomId { get; init; }
        public IReadOnlyList<LiveProductionParticipantContext> Participants { get; init; } = [];
    }

    public sealed record ProgramLowerThird
    {
        public required string Name { get; init; }
        public required string Title { get; init; }
        public required string Org { get; init; }
        public string? ParticipantId { get; init; }
    }

    public sealed record StudioLiveProductionPatch
    {
        public string? CaptionText { get; init; }
        public string? CaptionSpeaker { get; init; }
        public string? LowerThirdName { get; init; }
        public string? LowerThirdTitle { get; init; }
        public string? LowerThirdOrg { get; init; }
        public bool? Recording { get; init; }
        public bool? Streaming { get; init; }
        public string? OutputStatus { get; init; }
        public string? OutputSessionStatus { get; init; }
        public string? ZoomStatus { get; init; }
        public string? MeetingStateLabel { get; init; }
        public string? BreakoutRoomChangeHint { get; init; }
        public string? EngineAutoStopStatus { get; init; }
        public IReadOnlyList<LiveProductionParticipantContext>? Participants { get; init; }
    }

    public static StudioLiveProductionPatch MapSnapshotToStudioPatch(
        NativeMediaCoreStateSnapshot snapshot,
        LiveProductionSyncContext context)
    {
        var captionCue = snapshot.CaptionTrack.CurrentCue;
        var lowerThird = ResolveProgramLowerThird(snapshot, context);
        var recording = snapshot.Recording?.Active == true;
        var streaming = IsStreamingLive(snapshot);
        var outputStatus = MediaCoreBridgeService.SummarizeOutputs(snapshot);
        var outputSessionStatus = SummarizeOutputSession(snapshot);
        var meetingStateLabel = ResolveMeetingStateLabel(snapshot);
        var zoomStatus = meetingStateLabel switch
        {
            "in_meeting" => "Zoom Live",
            "idle" or "leaving" => "Zoom Offline",
            _ => null
        };

        return new StudioLiveProductionPatch
        {
            CaptionText = captionCue?.Text is { Length: > 0 } text ? text : null,
            CaptionSpeaker = captionCue?.Speaker is { Length: > 0 } speaker ? speaker : null,
            LowerThirdName = lowerThird.Name,
            LowerThirdTitle = lowerThird.Title,
            LowerThirdOrg = lowerThird.Org,
            Recording = recording,
            Streaming = streaming,
            OutputStatus = outputStatus,
            OutputSessionStatus = outputSessionStatus,
            ZoomStatus = zoomStatus,
            MeetingStateLabel = meetingStateLabel,
            BreakoutRoomChangeHint = ResolveBreakoutRoomChangeHint(snapshot, context.CurrentBreakoutRoomId),
            EngineAutoStopStatus = ResolveBreakoutRoomAutoStopStatus(snapshot, context.CurrentBreakoutRoomId),
            Participants = MapSnapshotParticipants(snapshot)
        };
    }

    /// <summary>
    /// Maps immutable media-core roster data into studio participant context.
    /// Mirrors <c>captureSnapshotMapper.ts</c>.
    /// </summary>
    public static IReadOnlyList<LiveProductionParticipantContext>? MapSnapshotParticipants(
        NativeMediaCoreStateSnapshot snapshot)
    {
        if (snapshot.Participants.Count == 0)
        {
            return null;
        }

        var meetingState = ResolveMeetingStateLabel(snapshot);
        if (!string.Equals(meetingState, "in_meeting", StringComparison.OrdinalIgnoreCase))
        {
            return [];
        }

        return MapRawParticipants(snapshot.Participants, snapshot.ActiveSpeakerId);
    }

    public static IReadOnlyList<LiveProductionParticipantContext> MapRawParticipants(
        IReadOnlyList<RawParticipantEvent> participants,
        string? activeSpeakerId = null)
    {
        var normalizedActiveSpeakerId = activeSpeakerId?.Trim() ?? string.Empty;

        return participants
            .Select(participant =>
            {
                var id = participant.UserId.Trim();
                var healthLabel = NormalizeFeedHealthLabel(participant);
                var roleLabel = string.IsNullOrWhiteSpace(participant.Role) ? "Guest" : participant.Role.Trim();
                var title = string.IsNullOrWhiteSpace(participant.Title)
                    ? roleLabel
                    : participant.Title.Trim();
                var isActiveSpeaker = !string.IsNullOrWhiteSpace(normalizedActiveSpeakerId)
                    ? id.Equals(normalizedActiveSpeakerId, StringComparison.Ordinal)
                    : participant.Talking == true;
                var audioLevel = participant.AudioLevel ?? (participant.Talking == true ? 78 : 22);
                audioLevel = Math.Clamp(audioLevel, 0, 100);

                return new LiveProductionParticipantContext
                {
                    Id = id,
                    Name = string.IsNullOrWhiteSpace(participant.DisplayName)
                        ? $"Zoom User {id}"
                        : participant.DisplayName.Trim(),
                    Title = title,
                    RoleLabel = roleLabel,
                    BreakoutRoomId = string.IsNullOrWhiteSpace(participant.BreakoutRoomId)
                        ? "main"
                        : participant.BreakoutRoomId.Trim(),
                    BreakoutRoomName = string.IsNullOrWhiteSpace(participant.BreakoutRoomName)
                        ? "Main room"
                        : participant.BreakoutRoomName.Trim(),
                    IsActiveSpeaker = isActiveSpeaker,
                    IsScreenSharing = participant.SharingScreen == true,
                    IsMuted = participant.Muted == true,
                    AudioLevel = audioLevel,
                    HealthLabel = healthLabel
                };
            })
            .ToList();
    }

    public static IReadOnlyList<LiveProductionParticipantContext> MapCaptureSnapshotParticipants(
        RawCaptureSnapshot snapshot) =>
        string.Equals(snapshot.MeetingState, "in_meeting", StringComparison.Ordinal)
            ? MapRawParticipants(snapshot.Participants, snapshot.ActiveSpeakerId)
            : [];

    /// <summary>
    /// Idle production readouts when no snapshot or engine context is available.
    /// </summary>
    public static StudioLiveProductionPatch CreateIdleProductionPatch() =>
        new()
        {
            Recording = false,
            Streaming = false,
            OutputStatus = "Outputs idle",
            OutputSessionStatus = "Outputs idle",
            ZoomStatus = "Zoom Offline",
            MeetingStateLabel = "idle",
            BreakoutRoomChangeHint = null
        };

    [Obsolete("Use CreateIdleProductionPatch instead.")]
    public static StudioLiveProductionPatch CreateIdleFallbackPatch() => CreateIdleProductionPatch();

    public static ProgramLowerThird ResolveProgramLowerThird(
        NativeMediaCoreStateSnapshot snapshot,
        LiveProductionSyncContext context)
    {
        var focus = ResolveSceneFocusParticipant(snapshot, context);
        if (focus is null)
        {
            var fallbackSpeaker = snapshot.CaptionTrack.CurrentCue?.Speaker;
            return new ProgramLowerThird
            {
                Name = fallbackSpeaker ?? snapshot.BrandKit.Name,
                Title = snapshot.BrandKit.Summary is { Length: > 0 } summary && !summary.Equals("Idle", StringComparison.Ordinal)
                    ? summary
                    : "Zoom-native production",
                Org = snapshot.BrandKit.Name
            };
        }

        return new ProgramLowerThird
        {
            Name = focus.Name,
            Title = string.IsNullOrWhiteSpace(focus.Title) ? focus.RoleLabel : focus.Title,
            Org = string.IsNullOrWhiteSpace(focus.BreakoutRoomName) ? focus.RoleLabel : focus.BreakoutRoomName,
            ParticipantId = focus.Id
        };
    }

    public static LiveProductionParticipantContext? ResolveSceneFocusParticipant(
        NativeMediaCoreStateSnapshot snapshot,
        LiveProductionSyncContext context)
    {
        if (context.Participants.Count == 0)
        {
            return null;
        }

        var routeParticipants = snapshot.RenderPlan.Routes
            .Select(route => ResolveRouteParticipant(route, context.Participants))
            .Where(participant => participant is not null)
            .Cast<LiveProductionParticipantContext>()
            .DistinctBy(participant => participant.Id)
            .ToList();

        if (routeParticipants.Count > 0)
        {
            return routeParticipants.FirstOrDefault(participant => participant.IsActiveSpeaker)
                   ?? routeParticipants[0];
        }

        if (context.ActiveSceneLayout is "host-focus" or "outro")
        {
            return context.Participants.FirstOrDefault(participant => participant.RoleLabel == "Host")
                   ?? context.Participants[0];
        }

        return context.Participants.FirstOrDefault(participant => participant.IsActiveSpeaker)
               ?? context.Participants[0];
    }

    public static bool IsStreamingLive(NativeMediaCoreStateSnapshot snapshot)
    {
        if (snapshot.OutputSenderSession.Status is "live" or "warning")
        {
            return snapshot.OutputSenderSession.ActiveSenderCount > 0;
        }

        return snapshot.OutputHealth.Any(item =>
            item.Destination is not "recording" &&
            item.Status is "live" or "warning");
    }

    public sealed record TransportReadoutLabels
    {
        public required string RecordStatValue { get; init; }
        public required string StreamStatValue { get; init; }
        public required string RecordHealthStatus { get; init; }
        public required string StreamHealthStatus { get; init; }
    }

    public static TransportReadoutLabels MapTransportReadouts(
        NativeMediaCoreStateSnapshot snapshot,
        bool recording,
        bool streaming,
        string programResolutionLabel)
    {
        var recordHealthStatus = ResolveRecordHealthStatus(snapshot, recording);
        var streamHealthStatus = ResolveStreamHealthStatus(snapshot, streaming);

        return new TransportReadoutLabels
        {
            RecordStatValue = SummarizeRecordStat(snapshot, recording),
            StreamStatValue = SummarizeStreamStat(snapshot, streaming, programResolutionLabel),
            RecordHealthStatus = recordHealthStatus,
            StreamHealthStatus = streamHealthStatus
        };
    }

    public static string SummarizeRecordStat(NativeMediaCoreStateSnapshot snapshot, bool recording)
    {
        if (!recording && snapshot.Recording?.Active != true)
        {
            return "Idle";
        }

        var path = ResolveRecordingPath(snapshot);
        if (path is { Length: > 0 })
        {
            return path;
        }

        if (snapshot.Recording?.Active == true)
        {
            return snapshot.Recording.Status;
        }

        return recording ? "Recording program" : "Idle";
    }

    public static string SummarizeStreamStat(
        NativeMediaCoreStateSnapshot snapshot,
        bool streaming,
        string programResolutionLabel)
    {
        var liveOutputs = snapshot.OutputHealth
            .Where(item =>
                !item.Destination.Equals("recording", StringComparison.OrdinalIgnoreCase) &&
                item.Status is "live" or "warning")
            .ToList();

        if (liveOutputs.Count == 0)
        {
            var liveSender = snapshot.OutputSenderSession.Senders.FirstOrDefault(sender =>
                sender.Status is "live" or "warning" or "starting");
            if (liveSender is null)
            {
                return streaming ? $"{programResolutionLabel} · starting" : "Idle";
            }

            return FormatStreamSenderLabel(liveSender, programResolutionLabel, snapshot.OutputProfile.TargetBitrateMbps);
        }

        if (liveOutputs.Count == 1)
        {
            return FormatStreamOutputLabel(liveOutputs[0], snapshot, programResolutionLabel);
        }

        return $"Live: {string.Join(", ", liveOutputs.Select(item => item.Destination.ToUpperInvariant()))}";
    }

    public static string? ResolveRecordingPath(NativeMediaCoreStateSnapshot snapshot)
    {
        if (snapshot.Recording?.ProgramPath is { Length: > 0 } programPath)
        {
            return programPath;
        }

        return ResolveRecordingArtifactPath(snapshot);
    }

    public static string? ResolveRecordingArtifactPath(NativeMediaCoreStateSnapshot snapshot)
    {
        const string prefix = "Recording artifact: ";
        var warning = snapshot.Warnings.FirstOrDefault(item => item.StartsWith(prefix, StringComparison.Ordinal));
        return warning is null ? null : warning[prefix.Length..].Trim();
    }

    public static string ResolveRecordHealthStatus(NativeMediaCoreStateSnapshot snapshot, bool recording)
    {
        var health = snapshot.OutputHealth.FirstOrDefault(item =>
            item.Destination.Equals("recording", StringComparison.OrdinalIgnoreCase));

        return health?.Status ?? (recording || snapshot.Recording?.Active == true ? "live" : "idle");
    }

    public static string ResolveStreamHealthStatus(NativeMediaCoreStateSnapshot snapshot, bool streaming)
    {
        var health = snapshot.OutputHealth.FirstOrDefault(item =>
            !item.Destination.Equals("recording", StringComparison.OrdinalIgnoreCase) &&
            (item.Destination.Equals("rtmp", StringComparison.OrdinalIgnoreCase) ||
             item.Destination.Equals("program", StringComparison.OrdinalIgnoreCase)));

        health ??= snapshot.OutputHealth.FirstOrDefault(item =>
            !item.Destination.Equals("recording", StringComparison.OrdinalIgnoreCase));

        return health?.Status ?? (streaming || IsStreamingLive(snapshot) ? "live" : "idle");
    }

    public static string SummarizeLocalOutputs(bool recording, bool streaming)
    {
        if (recording && streaming)
        {
            return "Recording and streaming";
        }

        if (recording)
        {
            return "Recording program";
        }

        if (streaming)
        {
            return "Streaming live";
        }

        return "Outputs idle";
    }

    public static string SummarizeOutputSession(NativeMediaCoreStateSnapshot snapshot)
    {
        if (snapshot.Recording?.Active == true)
        {
            var path = ResolveRecordingPath(snapshot) ?? snapshot.Recording.ProgramPath;
            return $"Recording {snapshot.Recording.Status} · {path}";
        }

        var liveOutputs = snapshot.OutputHealth
            .Where(item => item.Status is "live" or "warning")
            .Select(item => item.Destination.ToUpperInvariant())
            .Distinct()
            .ToList();

        if (liveOutputs.Count > 0)
        {
            return $"Live: {string.Join(", ", liveOutputs)}";
        }

        if (snapshot.EncoderSession.Status is "encoding" or "warning")
        {
            return $"Encoder {snapshot.EncoderSession.Lifecycle.Status}";
        }

        return "Outputs idle";
    }

    /// <summary>
    /// When the wire snapshot reports a different breakout room than the studio host room,
    /// return the status message used to auto-stop a running engine.
    /// </summary>
    public static string? ResolveBreakoutRoomAutoStopStatus(
        NativeMediaCoreStateSnapshot snapshot,
        string currentBreakoutRoomId)
    {
        if (string.IsNullOrWhiteSpace(snapshot.BreakoutRoomId) ||
            snapshot.BreakoutRoomId.Equals(currentBreakoutRoomId, StringComparison.Ordinal))
        {
            return null;
        }

        var roomLabel = string.IsNullOrWhiteSpace(snapshot.BreakoutRoomName)
            ? snapshot.BreakoutRoomId
            : snapshot.BreakoutRoomName;

        return $"Engine auto-stopped — breakout room changed to {roomLabel}";
    }

    /// <summary>
    /// When <see cref="NativeMediaCoreStateSnapshot.BreakoutRoomId"/> is present on the wire
    /// snapshot, compare it to the studio's current room and recommend an engine restart.
    /// </summary>
    public static string? ResolveBreakoutRoomChangeHint(
        NativeMediaCoreStateSnapshot snapshot,
        string currentBreakoutRoomId)
    {
        if (string.IsNullOrWhiteSpace(snapshot.BreakoutRoomId) ||
            snapshot.BreakoutRoomId.Equals(currentBreakoutRoomId, StringComparison.Ordinal))
        {
            return null;
        }

        var roomLabel = string.IsNullOrWhiteSpace(snapshot.BreakoutRoomName)
            ? snapshot.BreakoutRoomId
            : snapshot.BreakoutRoomName;

        return $"Breakout room changed to {roomLabel} — turn engine off before switching rooms.";
    }

    private static string NormalizeFeedHealthLabel(RawParticipantEvent participant)
    {
        if (participant.VideoOn == false)
        {
            return "video-off";
        }

        return participant.NetworkQuality switch
        {
            "low" => "low-resolution",
            "recovering" => "recovering",
            _ => "live"
        };
    }

    private static string? ResolveMeetingStateLabel(NativeMediaCoreStateSnapshot snapshot)
    {
        if (!string.IsNullOrWhiteSpace(snapshot.MeetingState))
        {
            return ZoomMediaSpineSnapshotMerger.NormalizeMeetingState(snapshot.MeetingState);
        }

        // Stub: infer meeting connectivity from the frame-source adapter until meetingState is on the wire.
        return snapshot.SourceSnapshot.Kind.Equals("zoom-sdk", StringComparison.Ordinal) &&
               snapshot.SourceSnapshot.Status is "subscribed" or "live"
            ? "in_meeting"
            : null;
    }

    private static LiveProductionParticipantContext? ResolveRouteParticipant(
        NativeMediaCoreResolvedRoute route,
        IReadOnlyList<LiveProductionParticipantContext> participants)
    {
        if (route.Mode.Equals("fixed", StringComparison.Ordinal) &&
            route.ParticipantId is { Length: > 0 } participantId)
        {
            return participants.FirstOrDefault(participant => participant.Id == participantId);
        }

        if (route.Mode.Equals("active-speaker", StringComparison.Ordinal))
        {
            return participants.FirstOrDefault(participant => participant.IsActiveSpeaker)
                   ?? (route.ParticipantId is { Length: > 0 } id
                       ? participants.FirstOrDefault(participant => participant.Id == id)
                       : null);
        }

        if (route.Mode.Equals("screen-share", StringComparison.Ordinal))
        {
            return participants.FirstOrDefault(participant => participant.IsScreenSharing);
        }

        return null;
    }

    private static string? FormatStreamOutputDetail(string? status, params string?[] details)
    {
        if (!string.Equals(status, "warning", StringComparison.OrdinalIgnoreCase) &&
            !string.Equals(status, "failed", StringComparison.OrdinalIgnoreCase))
        {
            return null;
        }

        var detail = details
            .FirstOrDefault(value => !string.IsNullOrWhiteSpace(value))?
            .Trim()
            .Replace("\r", " ", StringComparison.Ordinal)
            .Replace("\n", " ", StringComparison.Ordinal);
        if (string.IsNullOrWhiteSpace(detail))
        {
            return null;
        }

        const int maxLength = 120;
        return detail.Length <= maxLength ? detail : $"{detail[..(maxLength - 3)]}...";
    }

    private static string FormatStreamOutputLabel(
        NativeMediaCoreOutputHealth output,
        NativeMediaCoreStateSnapshot snapshot,
        string programResolutionLabel)
    {
        var sender = snapshot.OutputSenderSession.Senders.FirstOrDefault(item =>
            item.Destination.Equals(output.Destination, StringComparison.OrdinalIgnoreCase));
        var bitrateMbps = sender?.BitrateMbps ?? snapshot.OutputProfile.TargetBitrateMbps;
        var detail = FormatStreamOutputDetail(output.Status, output.Message, sender?.Warning ?? sender?.LastError);
        if (detail is not null)
        {
            return $"{output.Destination.ToUpperInvariant()} - {output.Status} - {bitrateMbps:0.#} Mbps - {detail}";
        }

        return $"{output.Destination.ToUpperInvariant()} · {output.Status} · {bitrateMbps:0.#} Mbps";
    }

    private static string FormatStreamSenderLabel(
        NativeMediaCoreOutputSender sender,
        string programResolutionLabel,
        double fallbackBitrateMbps) =>
        $"{sender.Destination.ToUpperInvariant()} · {sender.Status} · {sender.BitrateMbps:0.#} Mbps";
}
