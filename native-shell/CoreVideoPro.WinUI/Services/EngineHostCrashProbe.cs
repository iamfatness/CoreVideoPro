using System.Text.Json;
using CoreVideoPro.MediaCore.Services;

namespace CoreVideoPro.WinUI.Services;

// Explicit isolated verification mode: real compiled WinUI process + production
// bridge, no window/activation/preferences/camera registration or saved destinations.
internal static class EngineHostCrashProbe
{
    public static async Task RunAsync(string configPath)
    {
        using var configuration = JsonDocument.Parse(await File.ReadAllTextAsync(configPath));
        var config = configuration.RootElement;
        string Text(string name) => config.GetProperty(name).GetString()!;
        var report = Path.GetFullPath(Text("report"));
        var supervisor = new MediaCoreSupervisor(new()
        {
            EngineHost = new EngineHostClientOptions(Text("host"), Text("core"), Text("profile"))
        });
        await using var bridge = new MediaCoreBridgeService(supervisor);
        using var deadline = new CancellationTokenSource(TimeSpan.FromMinutes(3));
        await bridge.StartAsync(deadline.Token);
        if (config.TryGetProperty("takeControl", out var takeover) && takeover.GetBoolean())
            await supervisor.HostedControlAsync("take-control", deadline.Token);
        if (config.TryGetProperty("commands", out var commands))
        {
            foreach (var command in commands.EnumerateArray())
            {
                var payload = JsonSerializer.Deserialize<Dictionary<string, object?>>(command.GetRawText())!;
                using var response = await supervisor.ForwardHostedCommandAsync(payload, deadline.Token);
                if (response.RootElement.TryGetProperty("ok", out var ok) && !ok.GetBoolean())
                    throw new InvalidOperationException("Probe command rejected: " + response.RootElement.GetRawText());
            }
        }
        var seconds = config.TryGetProperty("seconds", out var duration) ? duration.GetInt32() : 5;
        if (seconds is < 1 or > 120) throw new ArgumentOutOfRangeException("seconds");
        var end = DateTimeOffset.UtcNow.AddSeconds(seconds);
        Directory.CreateDirectory(Path.GetDirectoryName(report)!);
        do
        {
            using var snapshot = await supervisor.ForwardHostedCommandAsync(new() { ["type"] = "snapshot" }, deadline.Token);
            var json = JsonSerializer.Serialize(new { uiPid = Environment.ProcessId, corePid = supervisor.HostedProcessId,
                readOnly = supervisor.HostedReadOnly, at = DateTimeOffset.UtcNow, snapshot = snapshot.RootElement });
            await File.WriteAllTextAsync(report + ".tmp", json, deadline.Token);
            File.Move(report + ".tmp", report, true);
            await Task.Delay(500, deadline.Token);
        } while (DateTimeOffset.UtcNow < end);
        if (config.TryGetProperty("shutdown", out var shutdown) && shutdown.GetBoolean())
            await supervisor.HostedControlAsync("shutdown", deadline.Token);
    }
}
