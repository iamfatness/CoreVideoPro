using System.Diagnostics;
using System.Text.Json;
using CoreVideoPro.MediaCore.Models;

namespace CoreVideoPro.MediaCore.Services;

public sealed partial class EngineHostServer
{
    internal static bool PreservesSpineIntent(JsonElement payload) =>
        payload.TryGetProperty("type", out var type) && type.ValueKind == JsonValueKind.String &&
        type.GetString() is "media-core-sync" or "native-media-core-sync" &&
        payload.TryGetProperty("commands", out var commands) && commands.ValueKind == JsonValueKind.Array &&
        commands.GetArrayLength() > 0 && commands.EnumerateArray().All(command =>
            command.ValueKind == JsonValueKind.Object &&
            command.TryGetProperty("type", out var commandType) && commandType.ValueKind == JsonValueKind.String &&
            commandType.GetString() is "set-audio-route-control" or "set-audio-monitor-control");

    private async Task RefreshSpineAsync(CancellationToken cancellationToken)
    {
        using var timer = new PeriodicTimer(TimeSpan.FromMilliseconds(500));
        try
        {
            while (await timer.WaitForNextTickAsync(cancellationToken).ConfigureAwait(false))
            {
                EngineHostSpineRefresh? refresh;
                lock (_state)
                {
                    // The attached console continues to publish current intent. Take
                    // over subscription refresh only when those updates stop.
                    refresh = !_stopping && _spineCorePid == supervisor.HostedProcessId &&
                        Stopwatch.GetElapsedTime(_spineIntentAt) >= TimeSpan.FromMilliseconds(1250)
                        ? _spineRefresh : null;
                }
                if (refresh is null) continue;

                using var deadline = CancellationTokenSource.CreateLinkedTokenSource(cancellationToken);
                deadline.CancelAfter(TimeSpan.FromMilliseconds(750));
                try
                {
                    var roster = await supervisor.GetZoomSnapshotAsync(deadline.Token).ConfigureAwait(false);
                    var payload = refresh.Build(roster);
                    if (!_commands.Wait(0)) continue;
                    try
                    {
                        Task<ZoomMediaSpineNativeSnapshot>? pending = null;
                        lock (_state)
                        {
                            if (ReferenceEquals(_spineRefresh, refresh) && !_stopping && _spineCorePid == supervisor.HostedProcessId)
                            {
                                if (payload is null) _spineRefresh = null;
                                else pending = supervisor.SyncZoomMediaSpineAsync(payload, Stopwatch.GetElapsedTime(_started).TotalMilliseconds, deadline.Token);
                            }
                        }
                        if (pending is not null) await pending.ConfigureAwait(false);
                    }
                    finally { _commands.Release(); }
                }
                catch (OperationCanceledException) when (deadline.IsCancellationRequested) { }
                catch (Exception ex) when (ex is InvalidOperationException or IOException or TimeoutException or JsonException)
                {
                    // A native rejection can mean the roster or capture changed
                    // after observation. The next tick obtains fresh facts.
                    var now = Environment.TickCount64;
                    if (now - Interlocked.Read(ref _lastClientLog) >= 1000)
                    {
                        Interlocked.Exchange(ref _lastClientLog, now);
                        DiagnosticLog.Write("engine-host.log", "Spine refresh deferred: " + ex.GetType().Name);
                    }
                }
            }
        }
        catch (OperationCanceledException) when (cancellationToken.IsCancellationRequested) { }
    }
}
