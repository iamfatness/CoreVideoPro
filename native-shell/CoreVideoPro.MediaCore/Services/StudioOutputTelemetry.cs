using CoreVideoPro.MediaCore.Contracts;
using CoreVideoPro.MediaCore.Models;

namespace CoreVideoPro.MediaCore.Services;

/// <summary>Compact observed output state; configuration and button intent are not delivery evidence.</summary>
public static class StudioOutputTelemetry
{
    public sealed record Readout(string Progress, string State, string Health);

    public static Readout Program(NativeMediaCoreStateSnapshot snapshot) => snapshot.Compositor.Status switch
    {
        "live" when Math.Max(snapshot.ProgramFrameCount, snapshot.Compositor.ProgramFrameCount) > 0 => new(Frames(Math.Max(snapshot.ProgramFrameCount, snapshot.Compositor.ProgramFrameCount)), "Rendering", "live"),
        "live" or "starting" or "preparing" => new("—", "Starting", "idle"),
        "warning" or "degraded" => new("—", "Degraded", "warning"),
        "failed" or "error" => new("—", "Failed", "warning"),
        _ => new("—", "Idle", "idle")
    };

    public static Readout Recording(NativeMediaCoreRecordingSession? recording) =>
        Map(recording?.Lifecycle, recording is null ? null : Math.Max(0, recording.TotalFramesWritten),
            "Recording", recording?.Active == true || recording?.Status is "starting" or "stopping", recording?.Status);

    public static Readout Streaming(NativeMediaCoreStateSnapshot snapshot)
    {
        var readout = Streaming(snapshot.OutputSenderSession.Senders);
        var aggregateHealth = LiveProductionSync.ResolveStreamHealthStatus(snapshot, streaming: false);
        if (aggregateHealth == "failed") return readout with { State = readout.State == "Partial" ? "Partial" : "Failed", Health = "warning" };
        if (aggregateHealth == "warning" && readout.Health != "warning") return readout with { State = "Degraded", Health = "warning" };
        return readout;
    }

    public static Readout Streaming(IReadOnlyList<NativeMediaCoreOutputSender> senders)
    {
        if (senders.Count == 0) return new("—", "Idle", "idle");
        var readouts = senders.Select(sender => Map(sender.Lifecycle, Math.Max(0, sender.FramesSent),
            "Streaming", sender.Status is not "idle" and not "stopped", sender.Status)).ToArray();
        var selected = readouts.OrderByDescending(item => item.Health == "warning" ? 3 : item.Health == "live" ? 2 : 1).First();
        if (readouts.Any(item => item.Health == "live") && readouts.Any(item => item.Health != "live"))
            selected = new("", "Partial", "warning");
        return selected with { Progress = Frames(senders.Sum(sender => (long)Math.Max(0, sender.FramesSent))) };
    }

    private static Readout Map(OutputLifecycle? lifecycle, long? frames, string producing, bool requested, string? legacyStatus)
    {
        var progress = frames is { } count ? Frames(count) : "—";
        if (lifecycle is null)
        {
            if (legacyStatus is "failed" or "interrupted") return new(progress, legacyStatus == "failed" ? "Failed" : "Interrupted", "warning");
            return new(progress, requested ? "Unverified" : "Idle", requested ? "warning" : "idle");
        }
        return lifecycle.State switch
        {
            "requested" or "preparing" or "starting" => new(progress, "Starting", "idle"),
            "producing" or "live" when !string.IsNullOrWhiteSpace(lifecycle.SessionId) && lifecycle.Health is "healthy" or "degraded" =>
                new(progress, lifecycle.Health == "degraded" ? "Degraded" : producing, lifecycle.Health == "degraded" ? "warning" : "live"),
            "stopping" => new(progress, "Stopping", "idle"),
            "finalizing" => new(progress, "Finalizing", "idle"),
            "completed" when lifecycle.Finalized => new(progress, "Completed", "idle"),
            "completed" or "failed" => new(progress, "Failed", "warning"),
            "interrupted" => new(progress, "Interrupted", "warning"),
            "idle" => new(progress, "Idle", "idle"),
            _ => new(progress, "Unverified", "warning")
        };
    }

    private static string Frames(long frames) => $"{frames:N0} frames";
}
