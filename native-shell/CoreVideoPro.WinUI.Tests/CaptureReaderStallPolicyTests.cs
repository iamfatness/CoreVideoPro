using CoreVideoPro.WinUI.Services;
using Xunit;

namespace CoreVideoPro.WinUI.Tests;

public sealed class CaptureReaderStallPolicyTests
{
    [Fact]
    public void Backoff_StartsAtBaseAndGrowsExponentially()
    {
        Assert.Equal(5000, CaptureReaderStallPolicy.BackoffMsForFailureCount(0));
        Assert.Equal(10000, CaptureReaderStallPolicy.BackoffMsForFailureCount(1));
        Assert.Equal(20000, CaptureReaderStallPolicy.BackoffMsForFailureCount(2));
        Assert.Equal(40000, CaptureReaderStallPolicy.BackoffMsForFailureCount(3));
    }

    [Fact]
    public void Backoff_IsCappedAndNeverOverflows()
    {
        Assert.Equal(CaptureReaderStallPolicy.MaxBackoffMs, CaptureReaderStallPolicy.BackoffMsForFailureCount(4));
        Assert.Equal(CaptureReaderStallPolicy.MaxBackoffMs, CaptureReaderStallPolicy.BackoffMsForFailureCount(20));
        // A pathological count must never wrap negative via the bit-shift.
        Assert.Equal(CaptureReaderStallPolicy.MaxBackoffMs, CaptureReaderStallPolicy.BackoffMsForFailureCount(int.MaxValue));
    }

    [Theory]
    [InlineData(0, false)]
    [InlineData(4, false)]
    [InlineData(5, true)]
    [InlineData(9, true)]
    public void ShouldGiveUp_OnlyAfterMaxConsecutiveFailures(int failures, bool expected)
    {
        Assert.Equal(expected, CaptureReaderStallPolicy.ShouldGiveUp(failures));
    }

    [Fact]
    public void GiveUp_IsReachableWithinBoundedTotalWait()
    {
        // The whole point: a dead source stops churning. Sum the backoffs up to give-up
        // and assert it's a small, bounded window (not "forever every 5s").
        long total = 0;
        for (var i = 0; i < CaptureReaderStallPolicy.MaxConsecutiveRestartFailures; i++)
        {
            total += CaptureReaderStallPolicy.BackoffMsForFailureCount(i);
        }

        Assert.True(CaptureReaderStallPolicy.ShouldGiveUp(CaptureReaderStallPolicy.MaxConsecutiveRestartFailures));
        Assert.True(total <= 5 * CaptureReaderStallPolicy.MaxBackoffMs);
    }
}
