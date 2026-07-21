using CoreVideoPro.MediaCore.Services;
using Xunit;

namespace CoreVideoPro.MediaCore.Tests;

/// <summary>
/// ISO-4 (spec §7): the pure ISO source-selection logic extracted from StudioViewModel —
/// the "Program + ISOs" switch gates it, only eligible-present sources arm, order is
/// deterministic, deduped, and capped.
/// </summary>
public sealed class IsoSourceSelectionResolverTests
{
    private static readonly string[] Eligible =
        ["zoom:p1", "zoom:p2", "capture:cam0", "capture:screen-1"];

    [Fact]
    public void Disabled_YieldsEmpty_ProgramOnly()
    {
        var result = IsoSourceSelectionResolver.Resolve(
            enabled: false,
            selectedSourceIds: ["zoom:p1", "capture:cam0"],
            eligiblePresentSourceIds: Eligible);

        Assert.Empty(result);
    }

    [Fact]
    public void Enabled_ReturnsSelectedSourcesInRosterOrder()
    {
        var result = IsoSourceSelectionResolver.Resolve(
            enabled: true,
            // selection order intentionally reversed vs the roster
            selectedSourceIds: ["capture:cam0", "zoom:p1"],
            eligiblePresentSourceIds: Eligible);

        // Ordered by the eligible (roster) list, not the selection order.
        Assert.Equal(["zoom:p1", "capture:cam0"], result);
    }

    [Fact]
    public void Enabled_DropsSelectedSourceThatIsNoLongerPresent()
    {
        // "zoom:p3" was selected but has left the room — no dangling writer arms.
        var result = IsoSourceSelectionResolver.Resolve(
            enabled: true,
            selectedSourceIds: ["zoom:p1", "zoom:p3"],
            eligiblePresentSourceIds: Eligible);

        Assert.Equal(["zoom:p1"], result);
    }

    [Fact]
    public void Enabled_WithNothingSelected_YieldsEmpty()
    {
        var result = IsoSourceSelectionResolver.Resolve(
            enabled: true, selectedSourceIds: [], eligiblePresentSourceIds: Eligible);

        Assert.Empty(result);
    }

    [Fact]
    public void Enabled_CapsAtMax()
    {
        var many = Enumerable.Range(0, 12).Select(i => $"zoom:p{i}").ToArray();
        var result = IsoSourceSelectionResolver.Resolve(
            enabled: true, selectedSourceIds: many, eligiblePresentSourceIds: many, max: 8);

        Assert.Equal(8, result.Count);
    }

    [Fact]
    public void CaptureSources_AreEligible()
    {
        var result = IsoSourceSelectionResolver.Resolve(
            enabled: true,
            selectedSourceIds: ["capture:cam0", "capture:screen-1"],
            eligiblePresentSourceIds: Eligible);

        Assert.Equal(["capture:cam0", "capture:screen-1"], result);
    }

    [Fact]
    public void ToLegacyParticipantIds_KeepsZoomStrippedAndDropsCapture()
    {
        var legacy = IsoSourceSelectionResolver.ToLegacyParticipantIds(
            ["zoom:p1", "capture:cam0", "zoom:p2"]);

        // Capture ids have no bare form; only zoom ids project to the legacy list.
        Assert.Equal(["p1", "p2"], legacy);
    }
}
