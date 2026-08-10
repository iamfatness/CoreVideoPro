using System.Reflection;
using CoreVideoPro.WinUI.Models;
using CoreVideoPro.WinUI.ViewModels;
using Xunit;

namespace CoreVideoPro.WinUI.Tests;

/// <summary>
/// Pins THE LAW (owner, stated three times during the 2026-08-09 live meeting): sources
/// appear in Show Input slots by OPERATOR action or by newcomer auto-assign ONLY. Three
/// separate background writers violated it that day — the auto-assign refill, the
/// EnsureAssignedSlotsForInShow fallback, and the dual-capture primary/secondary stuffer
/// that force-wrote the local webcams into slots 1-2 on every capture-fleet pass. The
/// slot model setters are now a logging choke point carrying an ambient writer reason
/// (<see cref="ShowInputWriteScope"/>) so the next mystery writer is identifiable from
/// launch.log alone.
/// </summary>
public sealed class ShowInputAssignmentLawTests
{
    [Fact]
    public void WriteScope_DefaultsToUntracked_ChainsOnNesting_AndRestores()
    {
        Assert.Equal(ShowInputWriteScope.Untracked, ShowInputWriteScope.Reason);

        using (ShowInputWriteScope.Enter("outer"))
        {
            Assert.Equal("outer", ShowInputWriteScope.Reason);

            using (ShowInputWriteScope.Enter("inner"))
            {
                // Nested scopes CHAIN so an outer entry point (e.g. control-api) stays
                // visible through the inner setter scopes (e.g. operator-picker).
                Assert.Equal("outer>inner", ShowInputWriteScope.Reason);
            }

            Assert.Equal("outer", ShowInputWriteScope.Reason);
        }

        Assert.Equal(ShowInputWriteScope.Untracked, ShowInputWriteScope.Reason);
    }

    [Fact]
    public void WriteScope_SurvivesAThrowingBody()
    {
        try
        {
            using var _ = ShowInputWriteScope.Enter("throwing");
            throw new InvalidOperationException("boom");
        }
        catch (InvalidOperationException)
        {
        }

        Assert.Equal(ShowInputWriteScope.Untracked, ShowInputWriteScope.Reason);
    }

    /// <summary>
    /// Name-level tripwire: the dual-capture slot stuffer
    /// (ApplyCaptureDeviceToShowInputSlot) wrote the primary/secondary capture devices
    /// straight into ShowInputs[0]/[1] — the THIRD writer behind "the Sources screen
    /// keeps putting the local webcams back into Inputs 1 and 2" (it stomped the
    /// operator's slots on every device-watcher event, Inputs-tab visit, and capture
    /// connect, then the roster save persisted the stomp). It was deleted, not disabled.
    /// If this test fails, someone re-introduced a method by that name — read the
    /// ApplyDualCaptureSelection comment in StudioViewModel before going further.
    /// </summary>
    [Fact]
    public void TheDualCaptureSlotStufferStaysDead()
    {
        var resurrected = typeof(StudioViewModel).GetMethod(
            "ApplyCaptureDeviceToShowInputSlot",
            BindingFlags.Instance | BindingFlags.Static | BindingFlags.Public | BindingFlags.NonPublic);
        Assert.Null(resurrected);
    }
}
