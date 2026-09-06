using CoreVideoPro.Control;
using CoreVideoPro.WinUI.Services;
using Xunit;

namespace CoreVideoPro.WinUI.Tests;

public sealed class StudioControlSurfaceCoverageTests
{
    // The WinUI adapter must handle every action declared in the transport-agnostic registry,
    // otherwise a remote client could invoke a documented action that returns "not implemented".
    [Fact]
    public void Adapter_HandlesEveryRegisteredAction()
    {
        var registered = ControlActionRegistry.Actions.Select(a => a.Id).ToHashSet(StringComparer.Ordinal);
        var supported = StudioControlSurface.SupportedActionIds;

        var missing = registered.Except(supported).OrderBy(x => x).ToList();
        Assert.True(missing.Count == 0, $"registry actions not handled by StudioControlSurface: {string.Join(", ", missing)}");

        // And no phantom ids in the adapter that the registry doesn't declare.
        var extra = supported.Except(registered).OrderBy(x => x).ToList();
        Assert.True(extra.Count == 0, $"StudioControlSurface handles unknown action ids: {string.Join(", ", extra)}");
    }
    [Fact]
    public void LowerThirdSetReportsRejectedSourceInsteadOfFalseSuccess()
    {
        var calls = 0;
        var result = StudioControlSurface.ApplyLowerThirdIntent(true, false,
            () => false, () => calls++, () => "Lower third needs a program source");
        Assert.False(result.Ok);
        Assert.Equal("Lower third needs a program source", result.Error);
        Assert.Equal(1, calls);
    }

    [Fact]
    public void LowerThirdSetAcceptsIntentWithoutRequiringCompletedAnimationAndIsIdempotent()
    {
        var enabled = false;
        var calls = 0;
        void Toggle() { calls++; enabled = !enabled; }
        Assert.True(StudioControlSurface.ApplyLowerThirdIntent(true, false,
            () => enabled, Toggle, () => "Lower third keyed in").Ok);
        Assert.True(StudioControlSurface.ApplyLowerThirdIntent(true, false,
            () => enabled, Toggle, () => "Lower third keyed in").Ok);
        Assert.Equal(1, calls);
        Assert.True(StudioControlSurface.ApplyLowerThirdIntent(false, false,
            () => enabled, Toggle, () => "Lower third keyed out").Ok);
        Assert.False(enabled);
        Assert.Equal(2, calls);
    }

}
