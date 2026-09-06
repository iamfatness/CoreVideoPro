using CoreVideoPro.MediaCore.Models;

namespace CoreVideoPro.MediaCore.Services;

/// <summary>Observed output activity is independent of operator intent.</summary>
public static class OutputLifecycleReadModel
{
    public static bool IsRecordingLive(NativeMediaCoreRecordingSession? recording) =>
        recording?.Lifecycle is { } lifecycle
            ? !string.IsNullOrWhiteSpace(lifecycle.SessionId) && lifecycle.State == "live" && lifecycle.Health is "healthy" or "degraded"
            : recording?.Active == true && recording.Status is "recording" or "warning";

    public static string RecordingStatus(NativeMediaCoreRecordingSession? recording) =>
        recording?.Lifecycle?.State switch
        {
            "starting" => "Recording starting — waiting for media",
            "live" => IsRecordingLive(recording) ? "Recording" : "Recording activity unverified",
            "stopping" or "finalizing" => "Recording finalizing — file is not ready yet",
            "completed" => recording.Lifecycle.Finalized ? "Recording finalized" : "Recording completion unverified",
            "failed" => "Recording failed",
            "interrupted" => "Recording interrupted",
            "idle" => "Recording idle",
            null => recording?.Status ?? "idle",
            _ => "Recording status unknown"
        };
}
