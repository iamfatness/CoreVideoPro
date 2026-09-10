using System.Text.RegularExpressions;
using CoreVideoPro.WinUI.Services;
using Xunit;

namespace CoreVideoPro.WinUI.Tests;

/// <summary>
/// Fault-injected BLOCKED PRESENT (beta slice, 2026-09-09).
///
/// G2 in <c>docs/production-realtime-execution-plan.md</c> wants shell presentation
/// fault-injected, and the property worth proving is that a present which will not return
/// cannot take UI control down with it.
///
/// THAT PROPERTY IS FALSE TODAY, and the honest thing is to say so rather than assert
/// something weaker under its name. Presents run on the UI thread from
/// <c>CompositionTarget.Rendering</c> (<c>VideoSurfaceHost.AttachGpuPresenter</c>), so a
/// blocked <c>Present</c> blocks the dispatcher, and <c>PresentationStageWatchdog</c> says
/// in its own header that it observes a blocked driver call but cannot cancel one. No test
/// can make that come out the other way while the present stays on the UI thread; a test
/// that appeared to would be testing something other than what it claims.
///
/// So these cases prove the two things that ARE true and do matter:
///  * The DIAGNOSIS path is independent of the blocked one. A present stuck in the driver
///    is detected, named by stage, and reported from a separate thread WHILE it is still
///    stuck — which is what turns "the app froze" on a tester's machine into a line in
///    launch.log naming the stage.
///  * The structural gap is pinned. If presents ever move off the UI thread, the last case
///    here fails and tells whoever moved them to come back and prove the real property.
/// </summary>
// xUnit1031 (do not block on tasks) is suppressed deliberately for this file: BLOCKING is
// the behaviour under test. An async rewrite would stop the test observing that the
// presenting thread is still stuck at the moment the watchdog reports it, which is the
// property these cases exist to prove.
#pragma warning disable xUnit1031
[Collection(PresentationFaultCollection.Name)]
public sealed class BlockedPresentFaultTests
{
    [Fact]
    public void ABlockedPresentIsDetectedAndNamedByStageWhileItIsStillBlocked()
    {
        using var block = PresentationFaultInjection.ArmBlockedPresentForTest(TimeSpan.FromSeconds(20));

        var reported = new TaskCompletionSource<(string Stage, long ElapsedMs)>(
            TaskCreationOptions.RunContinuationsAsynchronously);
        using var watchdog = new PresentationStageWatchdog(
            (stage, elapsed) => reported.TrySetResult((stage, elapsed)));

        // Exactly what Direct3D11InteropService does around the real present call.
        watchdog.Mark("present");
        var presenting = Task.Run(() =>
        {
            Assert.True(PresentationFaultInjection.Armed);
            PresentationFaultInjection.BeforePresent();
        });

        Assert.True(reported.Task.Wait(TimeSpan.FromSeconds(5)),
            "a present blocked for seconds was never reported by the stage watchdog");
        var (stage, elapsedMs) = reported.Task.Result;
        Assert.Equal("present", stage);
        Assert.True(elapsedMs >= 250, $"reported after only {elapsedMs}ms");

        // The report arrived from the watchdog's own timer thread while the present was
        // still inside the driver call — the observation does not depend on the blocked
        // path making progress. That is the whole point of the watchdog.
        Assert.False(presenting.IsCompleted);
        Assert.Equal(1, PresentationFaultInjection.PresentsBlocked);

        block.Release();
        Assert.True(presenting.Wait(TimeSpan.FromSeconds(5)), "the released present never returned");
        Assert.Equal(0, PresentationFaultInjection.PresentsBlocked);
    }

    [Fact]
    public void ABlockedPresentSeamAlwaysReleasesItselfSoItCannotWedgeTheProcess()
    {
        // A fault seam that can hang the process it exists to diagnose is not a diagnostic.
        // The hard timeout is the guarantee, and it is asserted rather than assumed.
        using var block = PresentationFaultInjection.ArmBlockedPresentForTest(TimeSpan.FromMilliseconds(300));

        var presenting = Task.Run(PresentationFaultInjection.BeforePresent);
        Assert.True(presenting.Wait(TimeSpan.FromSeconds(5)),
            "the injected present block did not self-release at its hard timeout");
        Assert.Equal(1, PresentationFaultInjection.PresentEntries);
    }

    [Fact]
    public void DisarmingReleasesAPresentThatIsStillBlocked()
    {
        var block = PresentationFaultInjection.ArmBlockedPresentForTest(TimeSpan.FromMinutes(5));
        var presenting = Task.Run(PresentationFaultInjection.BeforePresent);

        // Wait until the present is actually inside the block, then disarm. A failing test
        // must never be able to strand a blocked present for the rest of the run.
        var spun = SpinWait.SpinUntil(() => PresentationFaultInjection.PresentsBlocked == 1,
            TimeSpan.FromSeconds(5));
        Assert.True(spun, "the present never entered the injected block");

        block.Dispose();
        Assert.True(presenting.Wait(TimeSpan.FromSeconds(5)), "disarming did not release the present");
        Assert.False(PresentationFaultInjection.Armed);
    }

    [Fact]
    public void PresentsStillRunOnTheUiThread_SoABlockedPresentStillBlocksUiControl()
    {
        // THE HONEST GAP, pinned in code so it cannot quietly stay open.
        //
        // G2 wants a blocked present not to block UI control. It does block it, because
        // VideoSurfaceHost drives every present from CompositionTarget.Rendering, which
        // runs on the UI thread. This case asserts that this is STILL the shape, so that
        // whoever moves presents onto their own thread is told by a failing test to come
        // back and prove the real property with the seam above.
        var host = ReadControlSource("VideoSurfaceHost.xaml.cs");
        var collapsed = Regex.Replace(host, @"\s+", " ");

        Assert.Contains("CompositionTarget.Rendering += OnCompositionRendering", collapsed);
        Assert.Contains("private void OnCompositionRendering(object? sender, object e)", collapsed);
        Assert.Contains("TryPresentPendingSharedHandle();", collapsed);

        // And the watchdog still says out loud that it cannot cancel the blocked call —
        // if that claim is ever removed, this property has to be re-examined too.
        var watchdog = ReadServiceSource("PresentationStageWatchdog.cs");
        Assert.Contains("cannot", Regex.Replace(watchdog, @"\s+", " "));
    }

    private static string ReadControlSource(string fileName) =>
        ReadRepoSource(Path.Combine("Controls", fileName));

    private static string ReadServiceSource(string fileName) =>
        ReadRepoSource(Path.Combine("Services", fileName));

    private static string ReadRepoSource(string relativePath)
    {
        for (var directory = new DirectoryInfo(AppContext.BaseDirectory);
             directory is not null;
             directory = directory.Parent)
        {
            var candidate = Path.Combine(
                directory.FullName, "native-shell", "CoreVideoPro.WinUI", relativePath);
            if (File.Exists(candidate))
            {
                return File.ReadAllText(candidate);
            }
        }

        throw new FileNotFoundException($"Could not locate {relativePath} from the test output directory.");
    }
}
#pragma warning restore xUnit1031
