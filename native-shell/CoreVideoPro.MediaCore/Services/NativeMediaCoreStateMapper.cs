using System.Text.Json;
using CoreVideoPro.MediaCore.Models;

namespace CoreVideoPro.MediaCore.Services;

public static class NativeMediaCoreStateMapper
{
    public static bool IsNativeMediaCoreWireState(JsonElement value)
    {
        return value.ValueKind == JsonValueKind.Object &&
               value.TryGetProperty("health", out _) &&
               value.TryGetProperty("profile", out _) &&
               !value.TryGetProperty("diagnostics", out _);
    }

    public static NativeMediaCoreStateSnapshot MapNativeWireStateToSnapshot(
        IReadOnlyList<NativeMediaCoreCommand> commands,
        double elapsedMs,
        int frameNumber,
        NativeMediaCoreWireState wire)
    {
        var baseSnapshot = SyntheticMediaCore.SynthesizeSnapshot(commands, elapsedMs, frameNumber);
        var outputs = (wire.Outputs ?? baseSnapshot.Outputs)
            .Select(AsDestination)
            .Where(destination => destination is not null)
            .Select(destination => destination!)
            .ToList();

        var programFrameCount = Math.Max(
            wire.ProgramFrameCount ?? 0,
            wire.Health?.FrameCount ?? 0);
        programFrameCount = Math.Max(programFrameCount, wire.EncoderSession?.ProgramFrameCount ?? 0);

        var encoderSession = wire.EncoderSession ?? baseSnapshot.EncoderSession;
        var outputSenderSession = wire.OutputSenderSession ?? baseSnapshot.OutputSenderSession;
        var recording = wire.Recording is not null
            ? wire.Recording with
            {
                TotalBytesWritten = Math.Max(
                    wire.Recording.TotalBytesWritten,
                    wire.Health?.RecordingBytesWritten ?? 0)
            }
            : wire.Health?.RecordingArtifactPath is not null && baseSnapshot.Recording is not null
                ? baseSnapshot.Recording with
                {
                    TotalBytesWritten = Math.Max(
                        baseSnapshot.Recording.TotalBytesWritten,
                        wire.Health?.RecordingBytesWritten ?? 0)
                }
                : baseSnapshot.Recording;

        var compositorRenderer = wire.CompositorRenderer ??
                                 wire.Health?.Renderer ??
                                 wire.Profile?.Renderer ??
                                 "software";
        var programFrameHealth = NormalizeProgramFrameHealth(wire.Health?.ProgramFrameHealth);
        var compositorStatus = programFrameCount > 0
            ? programFrameHealth
            : compositorRenderer.Equals("software", StringComparison.Ordinal)
                ? "idle"
                : "idle";

        var warnings = baseSnapshot.Warnings.ToList();
        if (wire.Health?.RecordingArtifactPath is not null && recording?.Active == true)
        {
            warnings.Insert(0, $"Recording artifact: {wire.Health.RecordingArtifactPath}");
        }

        if (wire.Health?.Messages is not null)
        {
            foreach (var message in wire.Health.Messages.Where(message => !string.IsNullOrWhiteSpace(message)))
            {
                if (!warnings.Contains(message))
                {
                    warnings.Add(message);
                }
            }
        }

        var outputHealth = BuildOutputHealth(outputs, outputSenderSession, recording, encoderSession.Warnings);
        var programFramePreview = wire.ProgramFramePreview is not null
            ? DecodeProgramFramePreview(wire.ProgramFramePreview) ?? baseSnapshot.ProgramFramePreview
            : baseSnapshot.ProgramFramePreview;

        var programFrame = programFrameCount > 0
            ? new NativeMediaCoreProgramFrame
            {
                FrameNumber = programFrameCount,
                TimestampMs = elapsedMs,
                RenderPlanId = wire.RenderPlanId ?? baseSnapshot.ProgramFrame?.RenderPlanId ?? $"native-plan-{frameNumber}",
                SceneId = wire.SceneId ?? baseSnapshot.SceneId,
                Width = baseSnapshot.OutputProfile.Width,
                Height = baseSnapshot.OutputProfile.Height,
                Fps = baseSnapshot.OutputProfile.Fps,
                LayerCount = baseSnapshot.RenderPlan.Layers.Count,
                ColorGrade = baseSnapshot.RenderPlan.ColorGrade,
                Health = programFrameHealth
            }
            : null;

        var audioMixSession = wire.AudioMixSession ?? baseSnapshot.AudioMixSession;
        var captionTrack = wire.CaptionTrack ?? baseSnapshot.CaptionTrack;
        var brandKit = wire.BrandKit ?? baseSnapshot.BrandKit;
        var mergedWarnings = warnings
            .Concat(audioMixSession.Warnings.Where(warning => !warnings.Contains(warning)))
            .Concat(captionTrack.Warnings.Where(warning => !warnings.Contains(warning)))
            .Concat(brandKit.Warnings.Where(warning => !warnings.Contains(warning)))
            .Distinct()
            .ToList();

        var compositor = baseSnapshot.Compositor with
        {
            Status = compositorStatus,
            RenderPlanId = programFrame?.RenderPlanId,
            ProgramFrameCount = programFrameCount,
            LastFrame = programFrame
        };

        var programTransport = baseSnapshot.ProgramTransport with
        {
            Status = programFrame is null ? "idle" : "publishing",
            FrameNumber = programFrame?.FrameNumber,
            RenderPlanId = programFrame?.RenderPlanId,
            TimestampMs = programFrame?.TimestampMs
        };

        var diagnostics = baseSnapshot.Diagnostics with
        {
            GeneratedAtMs = elapsedMs,
            SceneId = wire.SceneId ?? baseSnapshot.SceneId,
            RouteCount = wire.RouteCount ?? baseSnapshot.RouteCount,
            FrameCount = baseSnapshot.FrameCount,
            ProgramFrameCount = programFrameCount,
            Outputs = outputs,
            OutputHealth = outputHealth,
            OutputSenderSession = outputSenderSession,
            Compositor = compositor,
            ProgramFrame = programFrame,
            ProgramFramePreview = programFramePreview,
            ProgramTransport = programTransport,
            EncoderSession = encoderSession,
            Recording = recording,
            AudioMixSession = audioMixSession,
            CaptionTrack = captionTrack,
            BrandKit = brandKit,
            Warnings = mergedWarnings
        };

        return baseSnapshot with
        {
            SceneId = wire.SceneId ?? baseSnapshot.SceneId,
            RouteCount = wire.RouteCount ?? baseSnapshot.RouteCount,
            ParticipantTransformCount = wire.TransformCount ?? baseSnapshot.ParticipantTransformCount,
            OverlayCount = wire.OverlayCount ?? baseSnapshot.OverlayCount,
            Outputs = outputs,
            IsoParticipantIds = wire.IsoParticipantIds ?? baseSnapshot.IsoParticipantIds,
            ProgramFrame = programFrame,
            ProgramFramePreview = programFramePreview,
            ProgramFrameCount = programFrameCount,
            Compositor = compositor,
            ProgramTransport = programTransport,
            EncoderSession = encoderSession,
            OutputSenderSession = outputSenderSession,
            OutputHealth = outputHealth,
            Recording = recording,
            AudioMixSession = audioMixSession,
            CaptionTrack = captionTrack,
            BrandKit = brandKit,
            Diagnostics = diagnostics,
            Warnings = mergedWarnings,
            MeetingState = wire.MeetingState ?? baseSnapshot.MeetingState,
            BreakoutRoomId = wire.BreakoutRoomId ?? baseSnapshot.BreakoutRoomId,
            BreakoutRoomName = wire.BreakoutRoomName ?? baseSnapshot.BreakoutRoomName,
            ActiveSpeakerId = wire.ActiveSpeakerId ?? baseSnapshot.ActiveSpeakerId,
            Participants = wire.Participants is { Count: > 0 } participants
                ? participants
                : baseSnapshot.Participants
        };
    }

    private static string? AsDestination(string value) =>
        value is "rtmp" or "ndi" or "srt" or "webrtc" or "recording" ? value : null;

    private static string NormalizeProgramFrameHealth(string? health) =>
        health switch
        {
            "degraded" => "degraded",
            "dropped" => "failed",
            "failed" => "failed",
            "error" => "failed",
            _ => "live"
        };

    private static NativeMediaCoreProgramFramePreview? DecodeProgramFramePreview(NativeMediaCoreProgramFramePreview preview)
    {
        if (preview.Bgra is { Length: > 0 })
        {
            return preview;
        }

        if (string.IsNullOrWhiteSpace(preview.BgraBase64))
        {
            return null;
        }

        try
        {
            var bgra = Convert.FromBase64String(preview.BgraBase64);
            if (bgra.Length != preview.Width * preview.Height * 4)
            {
                return null;
            }

            return preview with { Bgra = bgra };
        }
        catch (FormatException)
        {
            return null;
        }
    }

    private static IReadOnlyList<NativeMediaCoreOutputHealth> BuildOutputHealth(
        IReadOnlyList<string> outputs,
        NativeMediaCoreOutputSenderSession? senderSession,
        NativeMediaCoreRecordingSession? recording,
        IReadOnlyList<string> encoderWarnings)
    {
        var health = new List<NativeMediaCoreOutputHealth>();
        foreach (var destination in outputs)
        {
            if (destination == "recording")
            {
                var status = recording?.Status switch
                {
                    "failed" => "failed",
                    "warning" => "warning",
                    _ when recording?.WriterStatus == "warning" => "warning",
                    _ when recording?.Active == true => "live",
                    _ => "idle"
                };
                health.Add(new NativeMediaCoreOutputHealth
                {
                    Destination = destination,
                    Status = status,
                    Message = recording?.Error ?? recording?.Warning ?? recording?.ProgramPath ?? "Recording idle.",
                    DroppedFrames = recording?.TotalDroppedFrames ?? 0
                });
                continue;
            }

            var sender = senderSession?.Senders.FirstOrDefault(item => item.Destination == destination);
            health.Add(new NativeMediaCoreOutputHealth
            {
                Destination = destination,
                Status = sender?.Status switch
                {
                    "failed" => "failed",
                    "warning" => "warning",
                    "live" or "starting" => "live",
                    _ => "idle"
                },
                Message = sender?.Warning ?? $"{destination.ToUpperInvariant()} sender {sender?.Status ?? "idle"}.",
                DroppedFrames = 0
            });
        }

        foreach (var warning in encoderWarnings)
        {
            health.Add(new NativeMediaCoreOutputHealth
            {
                Destination = "recording",
                Status = "warning",
                Message = warning,
                DroppedFrames = 0
            });
        }

        return health;
    }
}
