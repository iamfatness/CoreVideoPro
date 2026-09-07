using CoreVideoPro.MediaCore.Models;
using CoreVideoPro.WinUI.Services;
using Xunit;

namespace CoreVideoPro.WinUI.Tests;

public sealed class ZoomJoinReconciliationTests
{
    private static RawCaptureSnapshot TimedOut() => new()
    {
        MeetingState = "error", Warnings = ["Timed out waiting for Zoom meeting join result."]
    };

    [Fact]
    public async Task LateNativeJoinedObservationResolvesOriginalTimeout()
    {
        var calls = 0;
        var joined = new RawCaptureSnapshot { MeetingState = "in-meeting", RawMediaActive = true };
        var result = await ZoomJoinReconciliation.ObserveLateJoinAsync(TimedOut(), _ =>
            Task.FromResult(++calls == 1 ? TimedOut() : joined), graceMs: 1000, pollIntervalMs: 1);
        Assert.Same(joined, result);
        Assert.Equal(2, calls);
    }

    [Fact]
    public async Task PersistentTimeoutRemainsFailureAndDoesNotWaitForeverForPoll()
    {
        var initial = TimedOut();
        var never = new TaskCompletionSource<RawCaptureSnapshot>();
        var result = await ZoomJoinReconciliation.ObserveLateJoinAsync(initial, _ => never.Task, graceMs: 30);
        Assert.Same(initial, result);
    }

    [Theory]
    [InlineData("SDK authentication failed")]
    [InlineData("Timed out waiting for Zoom SDK authentication.")]
    public async Task AuthenticationErrorsAreNotRetried(string warning)
    {
        var initial = new RawCaptureSnapshot { MeetingState = "error", Warnings = [warning] };
        var result = await ZoomJoinReconciliation.ObserveLateJoinAsync(initial,
            _ => throw new InvalidOperationException("Poll must not run"));
        Assert.Same(initial, result);
    }

    [Fact]
    public async Task JoiningWithoutJoinedEvidenceCannotBecomeSuccess()
    {
        var initial = TimedOut();
        var result = await ZoomJoinReconciliation.ObserveLateJoinAsync(initial,
            _ => Task.FromResult(new RawCaptureSnapshot { MeetingState = "joining" }), graceMs: 30, pollIntervalMs: 1);
        Assert.Same(initial, result);
    }
}
