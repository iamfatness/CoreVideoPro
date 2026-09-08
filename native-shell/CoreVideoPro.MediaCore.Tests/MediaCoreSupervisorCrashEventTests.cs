using System.Runtime.InteropServices;
using CoreVideoPro.MediaCore.Models;
using CoreVideoPro.MediaCore.Services;
using Xunit;

namespace CoreVideoPro.MediaCore.Tests;

public sealed class MediaCoreSupervisorCrashEventTests
{
    [Fact]
    public async Task RunningUnderBridgeGateUsesLifecycleStateWithoutProbingProcessWaitHandle()
    {
        await using var supervisor = new MediaCoreSupervisor();
        var bridge = new MediaCoreBridgeService(supervisor);
        using var process = new System.Diagnostics.Process();
        // Any attempt to query this process's OS state throws. The liveness
        // getter must use the already-published lifecycle state, including when
        // called under the bridge monitor as the periodic spine callback does.
        Assert.Throws<InvalidOperationException>(() => process.HasExited);
        void Set(string field, object? value) => typeof(MediaCoreSupervisor).GetField(field,
            System.Reflection.BindingFlags.NonPublic | System.Reflection.BindingFlags.Instance)!.SetValue(supervisor, value);
        var bridgeGate = typeof(MediaCoreBridgeService).GetField("_gate",
            System.Reflection.BindingFlags.NonPublic | System.Reflection.BindingFlags.Instance)!.GetValue(bridge)!;
        Set("_process", process); Set("_processAlive", true); Set("_stopped", false);
        try
        {
            lock (bridgeGate) { Assert.True(bridge.Running); Assert.Null(bridge.LastSnapshot); }
            Set("_processAlive", false);
            Assert.False(bridge.Running);
            Set("_processAlive", true); Set("_stopped", true);
            Assert.False(bridge.Running);
        }
        finally { Set("_process", null); Set("_processAlive", false); Set("_stopped", true); }
    }
    [Fact]
    public void ChildProcessEncoding_DoesNotEmitUtf8Bom()
    {
        Assert.Empty(MediaCoreSupervisor.ChildProcessEncoding.GetPreamble());
    }

    [Fact]
    public void ZoomRecoveryDefaults_AllowSdkTeardownCooldownAndBoundedRetries()
    {
        var options = new MediaCoreSupervisorOptions();

        Assert.Equal(3, options.ZoomRecoveryMaxAttempts);
        Assert.Equal(2500, options.ZoomRecoveryRetryDelayMs);
    }

    [Fact]
    public async Task ChildExit_IsCapturedAsCrashEventOnHealth()
    {
        // Spawn a child that exits immediately with a known code. MaxRestarts = 0 so
        // the supervisor records exactly one crash event and stops respawning.
        var (command, args) = QuickExitCommand(23);
        await using var supervisor = new MediaCoreSupervisor(new MediaCoreSupervisorOptions
        {
            Command = command,
            Args = args,
            MaxRestarts = 0,
            HandshakeRequestTimeoutMs = 1500,
            RequestTimeoutMs = 1000
        });

        var crashEventRaised = false;
        supervisor.HealthChanged += health =>
        {
            if (health.CrashEvents.Count > 0)
            {
                crashEventRaised = true;
            }
        };

        // The child never handshakes, so StartAsync surfaces a startup failure — expected.
        await Assert.ThrowsAnyAsync<InvalidOperationException>(() => supervisor.StartAsync());

        // Allow the Exited event to be raised and processed.
        for (var attempt = 0; attempt < 50 && supervisor.Health.CrashEvents.Count == 0; attempt++)
        {
            await Task.Delay(50);
        }

        var crashEvents = supervisor.Health.CrashEvents;
        Assert.NotEmpty(crashEvents);
        var crash = crashEvents[^1];
        Assert.Equal(1, crash.RestartCount);
        Assert.False(string.IsNullOrWhiteSpace(crash.At));
        // The child's exit code is captured (a real, non-null value from the OS).
        Assert.NotNull(crash.ExitCode);

        Assert.Equal(1, supervisor.Health.RestartCount);
        Assert.True(crashEventRaised);
    }

    [Fact]
    public async Task DefaultHealth_HasNoCrashEvents()
    {
        await using var supervisor = new MediaCoreSupervisor();
        Assert.Empty(supervisor.Health.CrashEvents);
        Assert.Equal(0, supervisor.Health.RestartCount);
    }

    private static (string Command, string[] Args) QuickExitCommand(int exitCode)
    {
        if (RuntimeInformation.IsOSPlatform(OSPlatform.Windows))
        {
            // Args are quoted per-token by the supervisor, so pass each token separately.
            return ("cmd.exe", ["/c", "exit", exitCode.ToString()]);
        }

        return ("/bin/sh", ["-c", $"exit {exitCode}"]);
    }
}
