using CoreVideoPro.WinUI.Services;
using Xunit;

namespace CoreVideoPro.WinUI.Tests;

/// <summary>
/// Plan 7b Task 10 — the ORDER <c>MainWindow.ApplyShowConfigAsync</c> runs its steps in, lifted
/// out of the window so it has a test seam at all (the window itself is not constructible here).
///
/// The order is not cosmetic. The mutation this file exists for is "restart the engine before
/// writing the effective config": the supervisor respawns node against the file on disk, so a
/// restart that happens before <c>materialize</c> boots the engine on the PREVIOUS config and the
/// operator's Save appears to have done nothing until the next app launch. Equally,
/// <c>replace-adapter</c> must come before <c>restart-engine</c> — the first host commands from
/// the fresh engine arrive within milliseconds of the restart, and an adapter still holding the
/// old look→scene presets would put the wrong scene on air.
/// </summary>
public sealed class OhgConfigApplyStepsTests
{
    [Fact]
    public void Order_WithARunningEngine_ValidatesThenWritesThenSwapsThenRestarts()
    {
        Assert.Equal(
            new[] { "validate", "materialize", "replace-adapter", "rebuild-page-vm", "restart-engine" },
            OhgConfigApplySteps.Order(engineRunning: true));
    }

    /// <summary>No engine at launch (no config, or the host could not be resolved): the config is
    /// still checked and written, but there is nothing to swap, rebuild or restart — the settings
    /// VM shows the restart-the-app message instead.</summary>
    [Fact]
    public void Order_WithNoEngine_ValidatesAndWritesOnly()
    {
        Assert.Equal(
            new[] { "validate", "materialize" },
            OhgConfigApplySteps.Order(engineRunning: false));
    }

    [Fact]
    public void Order_NeverRestartsBeforeItHasWrittenTheConfig()
    {
        var order = OhgConfigApplySteps.Order(engineRunning: true);

        Assert.True(
            order.ToList().IndexOf("materialize") < order.ToList().IndexOf("restart-engine"),
            "The effective config must be on disk before the engine is restarted onto it.");
        Assert.True(
            order.ToList().IndexOf("replace-adapter") < order.ToList().IndexOf("restart-engine"),
            "The adapter must carry the new presets before the fresh engine issues its first host command.");
    }

    /// <summary>Every step the window's switch knows how to run. A step added to the order without
    /// a case would silently do nothing.</summary>
    [Fact]
    public void EveryOrderedStepIsAKnownStep()
    {
        foreach (var step in OhgConfigApplySteps.Order(engineRunning: true))
        {
            Assert.Contains(step, OhgConfigApplySteps.AllSteps);
        }
    }
}
