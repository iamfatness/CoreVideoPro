using CoreVideoPro.WinUI.Services;
using Xunit;

namespace CoreVideoPro.WinUI.Tests;

/// <summary>
/// The classify-and-decide half of device-loss recovery, pinned without a GPU. Real device
/// loss cannot be provoked on demand without a TDR, so these cases are the contract the
/// GPU-side code is written against: which HRESULTs retire the shared device, which do not,
/// and how many times recovery may be attempted before the ladder gives up.
/// </summary>
public sealed class DeviceLossPolicyTests
{
    [Theory]
    [InlineData(unchecked((int)0x887A0005), true)]  // DXGI_ERROR_DEVICE_REMOVED
    [InlineData(unchecked((int)0x887A0007), true)]  // DXGI_ERROR_DEVICE_RESET
    [InlineData(unchecked((int)0x887A000A), false)] // WAS_STILL_DRAWING — a deferred present
    [InlineData(0x087A0001, false)]                 // DXGI_STATUS_OCCLUDED — success code
    [InlineData(unchecked((int)0x887A0006), false)] // DEVICE_HUNG is a REASON, never a present result
    [InlineData(unchecked((int)0x8007000E), false)] // E_OUTOFMEMORY — resource pressure, device is fine
    [InlineData(unchecked((int)0x80004005), false)] // E_FAIL
    [InlineData(0, false)]
    public void OnlyRemovedAndResetRetireTheDevice(int hr, bool expected) =>
        Assert.Equal(expected, DeviceLossPolicy.IsDeviceLoss(hr));

    [Fact]
    public void PresentFailuresThatAreNotDeviceLossStayOnThePerHandleInvalidationPath()
    {
        // The exact HRESULT PresentationAttemptTests pins as reaching the fallback handler
        // must be classified as device loss; the busy/occluded ones must not.
        Assert.True(DeviceLossPolicy.IsDeviceLoss(unchecked((int)0x887A0005)));
        Assert.False(DeviceLossPolicy.IsDeviceLoss(PresentationAttempt.StillDrawing));
    }

    [Theory]
    [InlineData(0, false)]                          // S_OK — device is healthy
    [InlineData(0x087A0001, false)]                 // any success code
    [InlineData(unchecked((int)0x887A0005), true)]
    [InlineData(unchecked((int)0x887A0006), true)]  // DEVICE_HUNG (our own TDR)
    [InlineData(unchecked((int)0x887A0020), true)]  // DRIVER_INTERNAL_ERROR
    public void RemovedReasonIsFatalOnlyWhenNegative(int reason, bool expected) =>
        Assert.Equal(expected, DeviceLossPolicy.IsRemovedReasonFatal(reason));

    [Fact]
    public void EveryRemovedReasonWeExpectIsNamedInWordsForTheSupportBundle()
    {
        // The reason code is the whole diagnosis after the fact — a bare hex value in a
        // tester's log is not enough to tell a TDR from a driver upgrade.
        Assert.Contains("S_OK", DeviceLossPolicy.DescribeRemovedReason(0));
        Assert.Contains("DEVICE_HUNG", DeviceLossPolicy.DescribeRemovedReason(DeviceLossPolicy.DeviceHung));
        Assert.Contains("driver upgrade", DeviceLossPolicy.DescribeRemovedReason(DeviceLossPolicy.DeviceRemoved));
        Assert.Contains("another application", DeviceLossPolicy.DescribeRemovedReason(DeviceLossPolicy.DeviceReset));
        Assert.Contains("DRIVER_INTERNAL_ERROR", DeviceLossPolicy.DescribeRemovedReason(DeviceLossPolicy.DriverInternalError));
        Assert.Contains("unrecognized", DeviceLossPolicy.DescribeRemovedReason(unchecked((int)0x88990001)));
    }

    [Fact]
    public void LadderEscalatesThenGivesUpAfterFiveConsecutiveFailures()
    {
        var now = DateTimeOffset.UnixEpoch;
        var policy = new DeviceLossPolicy.DeviceRecoveryPolicy();

        Assert.Equal(TimeSpan.FromMilliseconds(250), policy.NextDelay(now));
        Assert.Equal(TimeSpan.FromSeconds(1), policy.NextDelay(now));
        Assert.Equal(TimeSpan.FromSeconds(2), policy.NextDelay(now));
        Assert.Equal(TimeSpan.FromSeconds(5), policy.NextDelay(now));
        Assert.Equal(TimeSpan.FromSeconds(10), policy.NextDelay(now));
        Assert.Equal(5, policy.ConsecutiveFailures);

        // Sixth consecutive failure: give up rather than recreate forever on a wedged GPU.
        Assert.Null(policy.NextDelay(now));
    }

    [Fact]
    public void ADeviceThatNeverRunsCannotEarnAHealthyReset()
    {
        var now = DateTimeOffset.UnixEpoch;
        var policy = new DeviceLossPolicy.DeviceRecoveryPolicy();

        policy.NextDelay(now);
        policy.NextDelay(now);
        // No RecordRunning: a heartbeat racing a loss must not forgive budget.
        policy.RecordHealthy(now.AddHours(1));
        Assert.Equal(2, policy.ConsecutiveFailures);
    }

    [Fact]
    public void HealthyRunResetsTheBudgetOnlyAfterTheFullInterval()
    {
        var now = DateTimeOffset.UnixEpoch;
        var policy = new DeviceLossPolicy.DeviceRecoveryPolicy(healthyResetAfter: TimeSpan.FromSeconds(60));

        policy.NextDelay(now);
        policy.NextDelay(now);
        policy.RecordRunning(now);

        policy.RecordHealthy(now.AddSeconds(59));
        Assert.Equal(2, policy.ConsecutiveFailures);

        policy.RecordHealthy(now.AddSeconds(60));
        Assert.Equal(0, policy.ConsecutiveFailures);
    }

    [Fact]
    public void ARecoveredDeviceThatRunsHealthilyGetsAFullBudgetForTheNextLoss()
    {
        var now = DateTimeOffset.UnixEpoch;
        var policy = new DeviceLossPolicy.DeviceRecoveryPolicy();

        for (var i = 0; i < 4; i++) policy.NextDelay(now);
        policy.RecordRunning(now);
        policy.RecordHealthy(now.AddMinutes(5));
        Assert.Equal(0, policy.ConsecutiveFailures);

        // Back to the bottom of the ladder — an hour-later second loss is not the fifth.
        Assert.Equal(TimeSpan.FromMilliseconds(250), policy.NextDelay(now.AddHours(1)));
    }

    [Fact]
    public void NextDelayEndsTheRunningSpellSoAPostCrashHeartbeatCannotForgiveIt()
    {
        var now = DateTimeOffset.UnixEpoch;
        var policy = new DeviceLossPolicy.DeviceRecoveryPolicy();

        policy.RecordRunning(now);
        policy.NextDelay(now.AddMinutes(10));
        policy.RecordHealthy(now.AddMinutes(20));
        Assert.Equal(1, policy.ConsecutiveFailures);
    }

    [Fact]
    public void OperatorResetForgetsAnAbandonedHistory()
    {
        var now = DateTimeOffset.UnixEpoch;
        var policy = new DeviceLossPolicy.DeviceRecoveryPolicy();

        for (var i = 0; i < 6; i++) policy.NextDelay(now);
        Assert.Null(policy.NextDelay(now));

        policy.Reset();
        Assert.Equal(0, policy.ConsecutiveFailures);
        Assert.Equal(TimeSpan.FromMilliseconds(250), policy.NextDelay(now));
    }
}
