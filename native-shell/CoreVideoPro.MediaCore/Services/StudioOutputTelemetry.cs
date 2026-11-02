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

    public static string NdiStatus(
        bool enabled, IReadOnlyList<NativeMediaCoreOutputSender> senders, string? programName, string? refusalReason) =>
        !enabled
            ? string.IsNullOrWhiteSpace(refusalReason) ? "Off" : $"Unavailable \u00b7 {refusalReason}"
            : senders.FirstOrDefault(sender => sender.Destination == "ndi") switch
            {
                null => "Starting",
                { Status: "live" } => $"Live \u00b7 {(string.IsNullOrWhiteSpace(programName) ? "NDI" : programName)}" +
                    (senders.Any(sender => sender.Destination == "ndi-graphics" && sender.Status == "live") ? " + key" : string.Empty),
                { Status: "warning" or "failed" } sender =>
                    $"Warning \u00b7 {FirstText(sender.Warning, sender.LastError, sender.RuntimeDetail, sender.LastResultCode) ?? sender.Status}",
                _ => "Starting"
            };

    private static string? FirstText(params string?[] parts) =>
        parts.FirstOrDefault(static part => !string.IsNullOrWhiteSpace(part));

    public static Readout Streaming(NativeMediaCoreStateSnapshot snapshot)
    {
        var senders = CurrentSenders(StreamSenders(snapshot.OutputSenderSession.Senders));
        var readout = Streaming(senders);
        // Modern destination lifecycles are authoritative. Historical aggregate
        // health must not turn an intentionally ended destination into a current failure.
        if (senders.Count > 0 && senders.All(sender => sender.Lifecycle is not null))
        {
            if (readout.Health == "live" && StreamDegradationReadout.Format(snapshot) is not null)
                return readout with { State = "Degraded", Health = "warning" };
            return readout;
        }
        var aggregateHealth = LiveProductionSync.ResolveStreamHealthStatus(snapshot, streaming: false);
        if (aggregateHealth == "failed") return readout with { State = readout.State == "Partial" ? "Partial" : "Failed", Health = "warning" };
        if (aggregateHealth == "warning" && readout.Health != "warning") return readout with { State = "Degraded", Health = "warning" };
        return readout;
    }

    public static Readout Streaming(IReadOnlyList<NativeMediaCoreOutputSender> senders)
    {
        senders = CurrentSenders(StreamSenders(senders));
        if (senders.Count == 0) return new("—", "Idle", "idle");
        var readouts = senders.Select(sender => Map(sender.Lifecycle, Math.Max(0, sender.FramesSent),
            "Streaming", sender.Status is not "idle" and not "stopped", sender.Status)).ToArray();
        var selected = readouts.OrderByDescending(item => item.Health == "warning" ? 3 : item.Health == "live" ? 2 : 1).First();
        if (readouts.Any(item => item.Health == "live") && readouts.Any(item => item.Health != "live"))
            selected = new("", "Partial", "warning");
        return selected with { Progress = Frames(senders.Sum(sender => (long)Math.Max(0, sender.FramesSent))) };
    }

    // NDI is its own output with its own status label; it never feeds the stream readout.
    private static IReadOnlyList<NativeMediaCoreOutputSender> StreamSenders(IReadOnlyList<NativeMediaCoreOutputSender> senders) =>
        senders.Where(static sender => OutputDestinationKind.IsStream(sender.Destination)).ToArray();

    private static IReadOnlyList<NativeMediaCoreOutputSender> CurrentSenders(IReadOnlyList<NativeMediaCoreOutputSender> senders)
    {
        var current = senders.Where(sender => sender.Lifecycle is { } lifecycle
            ? lifecycle.DesiredActive || lifecycle.State is "producing" or "live" or "preparing" or "starting" or "stopping" or "finalizing"
            : sender.Status is "live" or "starting" or "warning").ToArray();
        return current.Length > 0 ? current : senders;
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
