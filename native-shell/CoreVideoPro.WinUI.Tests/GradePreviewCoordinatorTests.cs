using System.Collections.Concurrent;
using CoreVideoPro.MediaCore.Models;
using CoreVideoPro.MediaCore.Services;
using CoreVideoPro.WinUI.Services;
using CoreVideoPro.WinUI.ViewModels;
using Xunit;
namespace CoreVideoPro.WinUI.Tests;
public sealed class GradePreviewCoordinatorTests
{
    private sealed class Transport : IGradePreviewTransport
    {
        public event Action<GradePreviewObservation>? GradePreviewReceived;
        public event Action<MediaCoreHealth>? HealthChanged;
        public readonly ConcurrentQueue<(long Revision, bool Enabled)> Commands = new();
        public readonly TaskCompletionSource Release = new(TaskCreationOptions.RunContinuationsAsynchronously);
        public bool RefuseNext;
        public async Task SetGradePreviewAsync(string instanceId, string sourceId, long revision, bool enabled,
            MediaCoreColorGradeWire grade, CancellationToken cancellationToken = default)
        {
            Commands.Enqueue((revision, enabled));
            if (enabled && RefuseNext) { RefuseNext = false; throw new InvalidOperationException("grade-preview-overloaded"); }
            if (enabled && revision == 0) await Release.Task;
        }
        public void Emit(GradePreviewObservation value) => GradePreviewReceived?.Invoke(value);
        public void Recover() => HealthChanged?.Invoke(new() { Recovering = true });
    }
    private static async Task Until(Func<bool> condition)
    {
        // #906: the coordinator's first command rides a thread-pool timer, so a shared CI
        // runner whose pool is saturated can take well over 3 s to deliver it. This waits
        // for an event (no assertion is relaxed); a genuine hang still fails, just later.
        var end = DateTime.UtcNow.AddSeconds(30);
        while (!condition() && DateTime.UtcNow < end) await Task.Delay(10);
        Assert.True(condition(), "condition not reached within 30 s");
    }
    [Fact] public void TemporaryAdmissionRefusalRetriesTheSameDraftAndAcceptsItsTexture()
    {
        var bridge = new Transport { RefuseNext = true };
        bridge.Release.SetResult();
        var editor = new ColorGradeEditorViewModel("preview:lower-third", "Graphics", new() { Lut = "none" });
        var clock = new PreviewClock();
        using var coordinator = new GradePreviewCoordinator(bridge, editor, action => action(), clock);
        clock.Step(0);
        var first = Assert.Single(bridge.Commands);
        clock.Step(100);
        Assert.Equal(2, bridge.Commands.Count);
        Assert.Equal(first, bridge.Commands.Last());
        bridge.Emit(new() { InstanceId = editor.InstanceId, SourceId = editor.SourceId, Revision = first.Revision,
            Status = "ready", Texture = new() { SharedHandleHex = "0x1234", Width = 1920, Height = 1080 } });
        Assert.True(editor.HasPreview);
    }

    [Fact] public async Task CoalescesEditsAndSerializesCloseBehindPendingCommand()
    {
        var bridge = new Transport(); var editor = new ColorGradeEditorViewModel("p1", "Camera", new() { Lut = "none" });
        using var coordinator = new GradePreviewCoordinator(bridge, editor, action => action());
        await Until(() => bridge.Commands.Count == 1);
        for (var i = 1; i <= 20; ++i) editor.Exposure = i;
        await Task.Delay(150); Assert.Single(bridge.Commands);
        bridge.Release.SetResult();
        await Until(() => bridge.Commands.Any(command => command.Revision == 20 && command.Enabled));
        coordinator.Dispose();
        await Until(() => bridge.Commands.Any(command => !command.Enabled));
        Assert.False(bridge.Commands.Last().Enabled);
        var count = bridge.Commands.Count; await Task.Delay(650); Assert.Equal(count, bridge.Commands.Count);
    }
    [Fact] public async Task DisposalWaitsForPendingEnableAndUnsubscribesObservations()
    {
        var bridge = new Transport(); var editor = new ColorGradeEditorViewModel("p1", "Camera", new() { Lut = "none" });
        using var coordinator = new GradePreviewCoordinator(bridge, editor, action => action());
        await Until(() => bridge.Commands.Count == 1);
        coordinator.Dispose(); Assert.Single(bridge.Commands);
        var status = editor.PreviewStatus; bridge.Recover(); Assert.Equal(status, editor.PreviewStatus);
        bridge.Release.SetResult(); await Until(() => bridge.Commands.Count == 2);
        Assert.False(bridge.Commands.Last().Enabled);
        bridge.Emit(new() { InstanceId = editor.InstanceId, SourceId = editor.SourceId, Status = "unavailable", Reason = "late" });
        Assert.Equal(status, editor.PreviewStatus);
    }
    // The pointer-drag behaviour (newest edit wins, one send per display frame, no wait for a native
    // frame) is covered by GradePreviewMailboxTests.

    /// <summary>Manual clock: the coordinator's one-shot timer fires on <see cref="Step"/> once due.</summary>
    private sealed class PreviewClock : TimeProvider, ITimer
    {
        private TimerCallback? _tick;
        private object? _state;
        private long _milliseconds;
        private long? _due;

        public override long TimestampFrequency => 1000;
        public override long GetTimestamp() => _milliseconds;
        public override ITimer CreateTimer(TimerCallback callback, object? state, TimeSpan dueTime, TimeSpan period)
        {
            _tick = callback;
            _state = state;
            Change(dueTime, period);
            return this;
        }
        public void Step(int milliseconds)
        {
            _milliseconds += milliseconds;
            for (var guard = 0; guard < 100 && _tick is not null && _due is { } due && due <= _milliseconds; ++guard)
            {
                _due = null;
                _tick.Invoke(_state);
            }
        }
        public bool Change(TimeSpan dueTime, TimeSpan period)
        {
            _due = dueTime == Timeout.InfiniteTimeSpan ? null : _milliseconds + (long)Math.Ceiling(dueTime.TotalMilliseconds);
            return true;
        }
        public void Dispose() { _tick = null; _due = null; }
        public ValueTask DisposeAsync() { Dispose(); return ValueTask.CompletedTask; }
    }
}
