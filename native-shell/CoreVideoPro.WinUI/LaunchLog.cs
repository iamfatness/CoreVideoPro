using CoreVideoPro.MediaCore.Services;

namespace CoreVideoPro.WinUI;

internal static class LaunchLog
{
    // Best-effort logging, including calls from D3D presentation callbacks.
    internal static void Write(string message) => DiagnosticLog.Write("launch.log", message);

    internal static void WriteException(string context, Exception exception, string? requestId = null) =>
        DiagnosticLog.WriteException("launch.log", context, exception, requestId);
}
