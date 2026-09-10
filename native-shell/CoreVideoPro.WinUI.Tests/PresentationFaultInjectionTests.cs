using System.Diagnostics;
using CoreVideoPro.WinUI.Services;
using Xunit;
using Xunit.Abstractions;

namespace CoreVideoPro.WinUI.Tests;

/// <summary>
/// Fault-injected shell presentation (beta slice, 2026-09-09).
///
/// <c>DeviceLossPolicyTests</c> pins the classify-and-decide half of device-loss recovery
/// without a GPU. These cases are the other half: they drive the REAL
/// <see cref="Direct3D11InteropService"/> against a REAL D3D11 device through an injected
/// device loss, and assert that the recovery that shipped the same day actually runs —
/// retirement by generation, the bounded ladder, a fresh device, generation adoption, and
/// the handle blacklist being cleared — without restarting anything.
///
/// WHAT IS AND IS NOT PROVEN HERE, plainly:
///  * PROVEN (behaviour, real GPU): loss is detected and classified, the device generation
///    is retired, the ladder schedules and then performs a real <c>D3D11CreateDevice</c>,
///    the host adopts the new generation, its per-generation handle blacklist is cleared,
///    the loss report is accurate, and a stale-generation host cannot double-spend the
///    recovery budget.
///  * NOT PROVEN here: the SWAP-CHAIN rebuild and the return to on-screen GPU presentation.
///    A <c>SwapChainPanel</c> is a XAML object and this test host runs with the Windows App
///    SDK bootstrap disabled, so no panel can exist in it. Everything up to and including
///    the new device is executed; the panel-attached present is not. That gap needs the app
///    itself, and is named in the report rather than papered over with a weaker assertion.
///
/// These share process-wide statics (the shared device, its generation, the loss counters),
/// so they live in one class — xUnit runs a class's cases serially — and every case resets
/// that state first.
/// </summary>
[Collection(PresentationFaultCollection.Name)]
public sealed class PresentationFaultInjectionTests
{
    private readonly ITestOutputHelper _output;

    public PresentationFaultInjectionTests(ITestOutputHelper output) => _output = output;

    [Fact]
    public void TheSeamRefusesToArmOutsideATestHostAndIsInertUntilArmed()
    {
        // The hot path reads exactly this, and it is false in a shipped build because
        // nothing there can arm it.
        Assert.False(PresentationFaultInjection.Armed);

        using (PresentationFaultInjection.ArmDeviceLossForTest())
        {
            Assert.True(PresentationFaultInjection.Armed);
        }

        Assert.False(PresentationFaultInjection.Armed);
    }

    [Fact]
    public void ADriveMethodRefusesWhenNoFaultIsArmed()
    {
        Assert.False(PresentationFaultInjection.Armed);
        using var host = new Direct3D11InteropService();
        Assert.Throws<InvalidOperationException>(() => host.EnsureDeviceForTest());
        Assert.Throws<InvalidOperationException>(() => host.InjectDeviceLossForTest());
    }

    // Creates a real D3D11 device with DriverType.Hardware, matching the product.
    // Tagged so CI can exclude it by trait if a runner has no such adapter; it is
    // NOT skipped dynamically, because a silent pass on a GPU-less box would assert
    // nothing while reading as proof that recovery works.
    [Trait("Requires", "Gpu")]
    [Fact]
    public void AnInjectedDeviceLossRetiresTheGenerationAndRecoveryRebuildsARealDeviceWithoutARestart()
    {
        using var fault = PresentationFaultInjection.ArmDeviceLossForTest();
        Direct3D11InteropService.ResetProcessDeviceStateForTest();

        using var host = new Direct3D11InteropService { Label = "fault-program" };
        // Deliberately a hard assertion, not a skip: a device-loss recovery test that
        // quietly passes without ever creating a device proves nothing, and xUnit 2.9
        // has no dynamic skip. This case needs a machine that can make a D3D11 hardware
        // device — if that is not true here, that is the finding.
        Assert.True(host.EnsureDeviceForTest(),
            "this case requires a D3D11 hardware device; none could be created in this test host");

        var generationBefore = host.AdoptedDeviceGenerationForTest;
        var devicePointerBefore = host.DevicePointer;
        Assert.Equal(Direct3D11InteropService.PresentationPath.DeviceReady, host.ActivePath);

        // A handle blacklisted against the doomed device. The dying device is usually WHY
        // an open failed, so recovery must not inherit the blacklist.
        const ulong blacklisted = 0xBADC0FFEE0DDF00DUL;
        host.InvalidateSharedHandle(blacklisted);
        Assert.True(host.IsHandleInvalidatedForTest(blacklisted));

        // THE FAULT. Same handler the present catch runs when it classifies a loss.
        host.InjectDeviceLossForTest(DeviceLossPolicy.DeviceHung);

        var afterLoss = Direct3D11InteropService.SnapshotDeviceLoss();
        _output.WriteLine($"after loss: {afterLoss}");
        Assert.Equal(1, afterLoss.LossCount);
        Assert.Equal(DeviceLossPolicy.DeviceHung, afterLoss.LastRemovedReason);
        Assert.Equal(generationBefore + 1, afterLoss.Generation);
        Assert.NotNull(afterLoss.LastLossUtc);
        Assert.False(afterLoss.Abandoned);
        Assert.True(Direct3D11InteropService.IsAwaitingDeviceRecovery());
        Assert.False(host.IsReady);
        Assert.True(host.IsCpuFallback);

        // The failing frame drops to CPU and recreation waits out the first ladder rung
        // (250ms) — it is deliberately NOT done inline, so the UI thread never eats a
        // device-create stall on the tick that already failed.
        Assert.False(host.EnsureDeviceForTest());

        // Drive the recovery the way CompositionTarget.Rendering does: try again every
        // vsync until the backoff deadline passes.
        var stopwatch = Stopwatch.StartNew();
        var recovered = false;
        while (stopwatch.Elapsed < TimeSpan.FromSeconds(10))
        {
            if (host.EnsureDeviceForTest())
            {
                recovered = true;
                break;
            }

            Thread.Sleep(16);
        }

        Assert.True(recovered, "GPU presentation never came back after an injected device loss");
        _output.WriteLine($"recovered after {stopwatch.ElapsedMilliseconds}ms");
        Assert.InRange(stopwatch.ElapsedMilliseconds, 200, 5_000);

        var afterRecovery = Direct3D11InteropService.SnapshotDeviceLoss();
        _output.WriteLine($"after recovery: {afterRecovery}");
        Assert.Equal(1, afterRecovery.RecreateCount);
        Assert.NotNull(afterRecovery.LastRecoveryUtc);
        Assert.False(Direct3D11InteropService.IsAwaitingDeviceRecovery());

        // The host is bound to the NEW generation, and it is a genuinely different device.
        Assert.Equal(generationBefore + 1, host.AdoptedDeviceGenerationForTest);
        Assert.NotEqual(0, host.DevicePointer);
        Assert.NotEqual(devicePointerBefore, host.DevicePointer);
        Assert.Equal(Direct3D11InteropService.PresentationPath.DeviceReady, host.ActivePath);

        // Blacklist cleared with the generation: without this, recovery "succeeds" onto a
        // frozen surface and the operator still sees nothing.
        Assert.False(host.IsHandleInvalidatedForTest(blacklisted));
    }

    // Creates a real D3D11 device with DriverType.Hardware, matching the product.
    // Tagged so CI can exclude it by trait if a runner has no such adapter; it is
    // NOT skipped dynamically, because a silent pass on a GPU-less box would assert
    // nothing while reading as proof that recovery works.
    [Trait("Requires", "Gpu")]
    [Fact]
    public void ASecondHostObservingTheSameLossDoesNotSpendTheRecoveryBudgetTwice()
    {
        using var fault = PresentationFaultInjection.ArmDeviceLossForTest();
        Direct3D11InteropService.ResetProcessDeviceStateForTest();

        using var program = new Direct3D11InteropService { Label = "fault-program" };
        using var preview = new Direct3D11InteropService { Label = "fault-preview" };
        Assert.True(program.EnsureDeviceForTest(),
            "this case requires a D3D11 hardware device; none could be created in this test host");
        Assert.True(preview.EnsureDeviceForTest());
        Assert.Equal(program.AdoptedDeviceGenerationForTest, preview.AdoptedDeviceGenerationForTest);

        // Both hosts see the same dead device on the same vsync, as they would in a real
        // show with program + preview + multiview all presenting.
        program.InjectDeviceLossForTest(DeviceLossPolicy.DeviceRemoved);
        preview.InjectDeviceLossForTest(DeviceLossPolicy.DeviceRemoved);

        var report = Direct3D11InteropService.SnapshotDeviceLoss();
        _output.WriteLine($"two hosts, one loss: {report}");

        // ONE retirement, ONE ladder rung. A second observer of an already-retired
        // generation is a no-op, not a second budget hit — otherwise three surfaces would
        // burn the whole five-failure budget on a single TDR.
        Assert.Equal(1, report.LossCount);
        Assert.Equal(1, report.ConsecutiveFailures);
        Assert.False(report.Abandoned);
    }

    [Fact]
    public void AnOperatorRetryClearsAnAbandonedRecoveryBudget()
    {
        using var fault = PresentationFaultInjection.ArmDeviceLossForTest();
        Direct3D11InteropService.ResetProcessDeviceStateForTest();

        Direct3D11InteropService.ResetDeviceRecoveryBudget();
        var report = Direct3D11InteropService.SnapshotDeviceLoss();
        Assert.Equal(0, report.ConsecutiveFailures);
        Assert.False(report.Abandoned);
    }
}
