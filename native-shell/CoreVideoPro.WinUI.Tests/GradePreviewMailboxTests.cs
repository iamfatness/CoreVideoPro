using System.Collections.Concurrent;
using CoreVideoPro.MediaCore.Models;
using CoreVideoPro.MediaCore.Services;
using CoreVideoPro.WinUI.Services;
using CoreVideoPro.WinUI.ViewModels;
using Xunit;
namespace CoreVideoPro.WinUI.Tests;

/// <summary>The coordinator's latest-value mailbox (perf/designer-live-preview): newest edit wins, one command
/// in flight, at most one send per display frame, no wait for a native frame, 500 ms idle renewals.</summary>
public sealed class GradePreviewMailboxTests
{
    private sealed class Transport : IGradePreviewTransport
    {
        public event Action<GradePreviewObservation>? GradePreviewReceived;
        public event Action<MediaCoreHealth>? HealthChanged;
        public readonly ConcurrentQueue<(long Revision, bool Enabled, bool Renew)> Commands = new();
        public TaskCompletionSource? Hold;
        public Task SetGradePreviewAsync(string instanceId, string sourceId, long revision, bool enabled,
            MediaCoreColorGradeWire grade, CancellationToken cancellationToken = default)
        {
            Commands.Enqueue((revision, enabled, false));
            return Hold?.Task ?? Task.CompletedTask;
        }
        public Task RenewGradePreviewAsync(string instanceId, string sourceId, long revision, MediaCoreColorGradeWire grade,
            CancellationToken cancellationToken = default)
        {
            Commands.Enqueue((revision, true, true));
            return Task.CompletedTask;
        }
        public void Emit(GradePreviewObservation value) => GradePreviewReceived?.Invoke(value);
        public void Recover() => HealthChanged?.Invoke(new() { Recovering = true });
    }

    private static readonly System.Text.Json.JsonElement Empty = System.Text.Json.JsonSerializer.SerializeToElement(new { });
    private static NativeGraphicsPreview Draft(double x) => new("broadcast", 1, Empty, Empty, new(), new() { X = x });
    private static ColorGradeEditorViewModel Editor()
    {
        var editor = new ColorGradeEditorViewModel("preview:lower-third", "Graphics", new() { Lut = "none" });
        editor.SetLowerThirdPreview(new(null, Graphics: Draft(0)));
        return editor;
    }
    private static GradePreviewObservation Ready(ColorGradeEditorViewModel editor, long revision) => new() {
        InstanceId = editor.InstanceId, SourceId = editor.SourceId, Revision = revision, Status = "ready",
        Texture = new() { SharedHandleHex = "0x" + (0x1000 + revision).ToString("x"), Width = 1920, Height = 1080, Format = "B8G8R8A8_UNORM" } };
    private static IReadOnlyList<(long Revision, bool Enabled, bool Renew)> Sets(Transport bridge) => bridge.Commands.Where(c => !c.Renew).ToArray();

    [Fact]
    public void NewestEditWinsWhileACommandIsInFlightAndNoStaleRevisionIsSent()
    {
        var bridge = new Transport { Hold = new(TaskCreationOptions.RunContinuationsAsynchronously) };
        var editor = Editor(); var clock = new MailboxClock();
        using var coordinator = new GradePreviewCoordinator(bridge, editor, action => action(), clock);
        clock.Step(0);
        var first = Assert.Single(Sets(bridge)).Revision;
        for (var i = 1; i <= 20; ++i) { editor.SetLowerThirdPreview(new(null, Graphics: Draft(i)), continuous: true); clock.Step(8); }
        Assert.Single(Sets(bridge)); // one command in flight; edits only replace the pending draft
        var hold = bridge.Hold!; bridge.Hold = null; hold.SetResult();
        Until(() => Sets(bridge).Count == 2, clock);
        Assert.Equal(editor.Revision, Sets(bridge).Last().Revision);
        Assert.DoesNotContain(Sets(bridge), c => c.Revision > first && c.Revision < editor.Revision);
    }

    [Fact]
    public void ContinuousEditsAreSentOncePerDisplayFrameWithoutWaitingForANativeFrame()
    {
        var bridge = new Transport(); var editor = Editor(); var clock = new MailboxClock();
        using var coordinator = new GradePreviewCoordinator(bridge, editor, action => action(), clock);
        clock.Step(0);
        Assert.Single(Sets(bridge));
        // 120 Hz pointer edits for 100 ms, never a native frame: about one send per 16 ms frame.
        for (var i = 1; i <= 12; ++i) { editor.SetLowerThirdPreview(new(null, Graphics: Draft(i)), continuous: true); clock.Step(8); }
        var sends = Sets(bridge).Count - 1;
        Assert.InRange(sends, 5, 7);
        Assert.True(Sets(bridge).Select(c => c.Revision).SequenceEqual(Sets(bridge).Select(c => c.Revision).Order()), "revisions only move forward");
        clock.Step(16);
        Assert.Equal(editor.Revision, Sets(bridge).Last().Revision); // the newest edit is sent within a frame of the last move
    }

    [Fact]
    public void AnEditJustAfterASendWaitsOutTheRestOfTheFrame()
    {
        var bridge = new Transport(); var editor = Editor(); var clock = new MailboxClock();
        using var coordinator = new GradePreviewCoordinator(bridge, editor, action => action(), clock);
        clock.Step(0);
        editor.SetLowerThirdPreview(new(null, Graphics: Draft(1)), continuous: true);
        clock.Step(1);
        Assert.Single(Sets(bridge));
        clock.Step(14);
        Assert.Single(Sets(bridge));
        clock.Step(1);
        Assert.Equal(2, Sets(bridge).Count);
        Assert.Equal(editor.Revision, Sets(bridge).Last().Revision);
    }

    [Fact]
    public void AParkedDraftOnlyRenewsItsLeaseEveryHalfSecond()
    {
        var bridge = new Transport(); var editor = Editor(); var clock = new MailboxClock();
        using var coordinator = new GradePreviewCoordinator(bridge, editor, action => action(), clock);
        clock.Step(0);
        for (var i = 0; i < 31; ++i) clock.Step(16); // 496 ms
        Assert.Single(bridge.Commands);
        Assert.Equal(1, clock.Wakeups); // the first send only; parked: the timer sleeps until the renewal, no per-frame wakeups
        clock.Step(4);
        Assert.Equal(2, bridge.Commands.Count);
        Assert.True(bridge.Commands.Last().Renew);
        clock.Step(499);
        Assert.Equal(2, bridge.Commands.Count);
        clock.Step(1);
        Assert.Equal(3, bridge.Commands.Count);
    }

    [Fact]
    public void FramesNeverGoBackwardsWhenAnOlderRevisionArrivesLate()
    {
        var bridge = new Transport(); var editor = Editor(); var clock = new MailboxClock();
        using var coordinator = new GradePreviewCoordinator(bridge, editor, action => action(), clock);
        clock.Step(0);
        var older = editor.Revision;
        editor.SetLowerThirdPreview(new(null, Graphics: Draft(5)), continuous: true);
        var newer = editor.Revision;
        bridge.Emit(Ready(editor, newer));
        Assert.Equal((ulong)(0x1000 + newer), editor.NativeSurface.PendingSharedHandle!.NtHandle);
        bridge.Emit(Ready(editor, older));
        Assert.Equal((ulong)(0x1000 + newer), editor.NativeSurface.PendingSharedHandle!.NtHandle);
    }

    private static void Until(Func<bool> condition, MailboxClock clock)
    {
        var end = DateTime.UtcNow.AddSeconds(30);
        while (!condition() && DateTime.UtcNow < end) { Thread.Sleep(5); clock.Step(0); }
        Assert.True(condition(), "condition not reached within 30 s");
    }

    /// <summary>Manual clock whose one-shot timer fires on <see cref="Step"/> when due.</summary>
    private sealed class MailboxClock : TimeProvider, ITimer
    {
        private readonly object _gate = new();
        private TimerCallback? _tick; private object? _state;
        private long _milliseconds; private long? _due;
        public int Wakeups { get; private set; }
        public override long TimestampFrequency => 1000;
        public override long GetTimestamp() { lock (_gate) return _milliseconds; }
        public override ITimer CreateTimer(TimerCallback callback, object? state, TimeSpan dueTime, TimeSpan period)
        {
            _tick = callback; _state = state; Change(dueTime, period); return this;
        }
        public bool Change(TimeSpan dueTime, TimeSpan period)
        {
            lock (_gate) _due = dueTime == Timeout.InfiniteTimeSpan ? null : _milliseconds + (long)Math.Ceiling(dueTime.TotalMilliseconds);
            return true;
        }
        public void Step(int milliseconds)
        {
            lock (_gate) _milliseconds += milliseconds;
            for (var guard = 0; guard < 100; ++guard) {
                TimerCallback? tick;
                lock (_gate) { if (_due is not { } due || due > _milliseconds || _tick is null) return; _due = null; tick = _tick; }
                ++Wakeups; tick(_state);
            }
        }
        public void Dispose() { lock (_gate) { _tick = null; _due = null; } }
        public ValueTask DisposeAsync() { Dispose(); return ValueTask.CompletedTask; }
    }
}
