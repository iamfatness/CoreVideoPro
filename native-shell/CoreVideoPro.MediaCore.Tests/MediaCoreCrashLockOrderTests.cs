using System.Diagnostics;
using System.Reflection;
using CoreVideoPro.MediaCore.Services;
using Xunit;

namespace CoreVideoPro.MediaCore.Tests;

public sealed class MediaCoreCrashLockOrderTests
{
    [Fact]
    public async Task CrashNotificationAllowsBridgeToAcquireSupervisorWhileHoldingItsGate()
    {
        // No child is started: invoke the exit path for an inert Process and
        // exhaust recovery immediately. The test owns only in-memory objects.
        await using var supervisor = new MediaCoreSupervisor(new MediaCoreSupervisorOptions { MaxRestarts = 0 });
        using var exited = new Process();
        Field(supervisor, "_process").SetValue(supervisor, exited);
        Field(supervisor, "_stopped").SetValue(supervisor, false);
        var supervisorGate = Field(supervisor, "_gate").GetValue(supervisor)!;
        using var bridgeHeld = new ManualResetEventSlim();
        using var notificationEntered = new ManualResetEventSlim();
        var callbackHeldSupervisorGate = false;
        supervisor.HealthChanged += health =>
        {
            if (!health.Recovering) return;
            callbackHeldSupervisorGate = Monitor.IsEntered(supervisorGate);
            notificationEntered.Set();
        };
        await using var bridge = new MediaCoreBridgeService(supervisor);
        var bridgeGate = Field(bridge, "_gate").GetValue(bridge)!;

        var concurrentSync = Task.Run(() =>
        {
            lock (bridgeGate)
            {
                bridgeHeld.Set();
                if (!notificationEntered.Wait(TimeSpan.FromSeconds(5))) return false;
                // This is the lock required by Running and atomic spine submit.
                // A bounded acquisition makes the old deadlock fail the test
                // without stranding either worker or hanging test teardown.
                var acquired = Monitor.TryEnter(supervisorGate, TimeSpan.FromSeconds(1));
                if (acquired) Monitor.Exit(supervisorGate);
                return acquired;
            }
        });
        Assert.True(bridgeHeld.Wait(TimeSpan.FromSeconds(5)));
        var notification = Task.Run(() => typeof(MediaCoreSupervisor)
            .GetMethod("OnChildExited", BindingFlags.Instance | BindingFlags.NonPublic)!
            .Invoke(supervisor, [exited, EventArgs.Empty]));
        Assert.True(await concurrentSync.WaitAsync(TimeSpan.FromSeconds(10)));
        await notification.WaitAsync(TimeSpan.FromSeconds(10));
        Assert.False(callbackHeldSupervisorGate);
        Assert.Equal(1, supervisor.Health.RestartCount);
    }

    [Fact]
    public async Task RecoverySubscriberCanReadHealthOnAnotherThreadAndStopWithoutRespawn()
    {
        await using var supervisor = new MediaCoreSupervisor(new MediaCoreSupervisorOptions
        {
            MaxRestarts = 1,
            Command = "must-not-respawn-after-subscriber-stop"
        });
        // Stop needs a real exited Process handle, but this synthetic child never
        // loads the core, cameras, Zoom, or any app state.
        using var exited = Process.Start(new ProcessStartInfo("node")
        {
            ArgumentList = { "-e", "process.exit(0)" },
            UseShellExecute = false,
            CreateNoWindow = true
        })!;
        await exited.WaitForExitAsync().WaitAsync(TimeSpan.FromSeconds(5));
        Field(supervisor, "_process").SetValue(supervisor, exited);
        Field(supervisor, "_stopped").SetValue(supervisor, false);
        var observedRecovering = false;
        var statuses = new List<string>();
        supervisor.StatusChanged += statuses.Add;
        supervisor.HealthChanged += health =>
        {
            if (!health.Recovering) return;
            observedRecovering = Task.Run(() => supervisor.Health.Recovering)
                .WaitAsync(TimeSpan.FromSeconds(2)).GetAwaiter().GetResult();
            supervisor.Stop();
        };
        typeof(MediaCoreSupervisor).GetMethod("OnChildExited", BindingFlags.Instance | BindingFlags.NonPublic)!
            .Invoke(supervisor, [exited, EventArgs.Empty]);
        Assert.True(observedRecovering);
        Assert.DoesNotContain(statuses, status => status.Contains("recovering", StringComparison.OrdinalIgnoreCase));
        Assert.True(supervisor.Health.Stopped);
        Assert.False(supervisor.Health.Recovering);
        Assert.Null(Field(supervisor, "_process").GetValue(supervisor));
    }

    private static FieldInfo Field(object instance, string name) => instance.GetType()
        .GetField(name, BindingFlags.Instance | BindingFlags.NonPublic)!;
}
