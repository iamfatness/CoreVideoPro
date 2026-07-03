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
}
