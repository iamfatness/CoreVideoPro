using CoreVideoPro.MediaCore.Services;
using Xunit;

namespace CoreVideoPro.MediaCore.Tests;

/// <summary>
/// ISO-4 (spec §6) disk pre-flight math: (program + Σ ISO bitrates) × duration vs free
/// space on the target volume — ample / low / insufficient, ported from the TS
/// isoRecording/diskSpace estimate.
/// </summary>
public sealed class IsoDiskPreflightTests
{
    private const long GB = 1_000_000_000L;

    [Fact]
    public void Ample_WhenPlentyOfHeadroom()
    {
        // 1 program + 3 ISO ≈ 8 + 18.576 ≈ 26.6 Mbps; 500 GB is hours of headroom.
        var result = IsoDiskPreflight.Evaluate(
            programBitrateMbps: 8, isoSourceCount: 3, availableBytes: 500 * GB);

        Assert.Equal(IsoDiskPreflightLevel.Ample, result.Level);
        Assert.False(result.ShouldBlock);
        Assert.False(result.ShouldWarn);
        Assert.Equal(string.Empty, result.Message);
    }

    [Fact]
    public void Low_WarnsButDoesNotBlock_WhenUnderPlanningWindow()
    {
        // ~26.6 Mbps ≈ 3.3 MB/s. ~4 GB holds ~20 min < the 30-min planning window but
        // well above the 5-min sane minimum → warn, proceed.
        var result = IsoDiskPreflight.Evaluate(
            programBitrateMbps: 8, isoSourceCount: 3, availableBytes: 4 * GB);

        Assert.Equal(IsoDiskPreflightLevel.Low, result.Level);
        Assert.True(result.ShouldWarn);
        Assert.False(result.ShouldBlock);
        Assert.Contains("Low disk space", result.Message);
    }

    [Fact]
    public void Insufficient_BlocksArming_WhenUnderSaneMinimum()
    {
        // ~26.6 Mbps ≈ 3.3 MB/s. 0.5 GB holds ~2.5 min < the 5-min sane minimum → block.
        var result = IsoDiskPreflight.Evaluate(
            programBitrateMbps: 8, isoSourceCount: 3, availableBytes: GB / 2);

        Assert.Equal(IsoDiskPreflightLevel.Insufficient, result.Level);
        Assert.True(result.ShouldBlock);
        Assert.Contains("Not enough disk space", result.Message);
        Assert.Contains("reduce ISO sources", result.Message);
    }

    [Fact]
    public void Insufficient_WhenVolumeIsFull()
    {
        var result = IsoDiskPreflight.Evaluate(
            programBitrateMbps: 8, isoSourceCount: 0, availableBytes: 0);

        Assert.Equal(IsoDiskPreflightLevel.Insufficient, result.Level);
        Assert.True(result.ShouldBlock);
    }

    [Fact]
    public void IsoCount_RaisesTheCombinedRate()
    {
        var programOnly = IsoDiskPreflight.Evaluate(8, 0, 500 * GB);
        var withIsos = IsoDiskPreflight.Evaluate(8, 4, 500 * GB);

        Assert.Equal(8, programOnly.CombinedBitrateMbps);
        Assert.True(withIsos.CombinedBitrateMbps > programOnly.CombinedBitrateMbps);
        Assert.Equal(4, withIsos.IsoSourceCount);
    }

    [Fact]
    public void ProgramOnly_AmpleVolume_IsNotBlockedByIsoAudioOverhead()
    {
        // Program-only (ISO off) never carries the per-ISO estimate.
        var result = IsoDiskPreflight.Evaluate(18, 0, 50 * GB);
        Assert.Equal(IsoDiskPreflightLevel.Ample, result.Level);
    }
}
