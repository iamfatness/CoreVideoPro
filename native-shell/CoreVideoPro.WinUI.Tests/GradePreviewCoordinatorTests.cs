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
        public async Task SetGradePreviewAsync(string instanceId, string sourceId, long revision, bool enabled,
            MediaCoreColorGradeWire grade, CancellationToken cancellationToken = default)
        {
            Commands.Enqueue((revision, enabled));
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
    [Fact]
    public void GraphicsDragCoalescesPointerEditsUntilNativeCanPublish()
    {
        var bridge = new Transport();
        var editor = new ColorGradeEditorViewModel("preview:lower-third", "Graphics", new() { Lut = "none" });
        var empty = System.Text.Json.JsonSerializer.SerializeToElement(new { });
        var draft = new NativeGraphicsPreview("broadcast", 1, empty, empty, new());
        editor.SetLowerThirdPreview(new(null, Graphics: draft));
        var clock = new PreviewClock();
        using var coordinator = new GradePreviewCoordinator(bridge, editor, action => action(), clock);
        clock.Step(0);
        var first = Assert.Single(bridge.Commands).Revision;
        editor.SetLowerThirdPreview(new(null, Graphics: draft with { Placement = new() { X = 12 } }), continuous: true);
        editor.SetLowerThirdPreview(new(null, Graphics: draft with { Placement = new() { X = 24 } }), continuous: true);
        clock.Step(100);
        Assert.Single(bridge.Commands);

        bridge.Emit(new() { InstanceId = editor.InstanceId, SourceId = editor.SourceId, Revision = first,
            Status = "ready", Texture = new() { SharedHandleHex = "0x1234", Width = 1920, Height = 1080 } });
        clock.Step(100);
        Assert.Equal(editor.Revision, bridge.Commands.Last().Revision);
        Assert.Equal(2, bridge.Commands.Count);
        Assert.True(editor.HasPreview);

        editor.SetLowerThirdPreview(new(null, Graphics: draft with { Placement = new() { X = 48 } }), continuous: true);
        clock.Step(100);
        Assert.Equal(2, bridge.Commands.Count);
        clock.Step(400);
        Assert.Equal(editor.Revision, bridge.Commands.Last().Revision);
        Assert.Equal(3, bridge.Commands.Count);
    }

    private sealed class PreviewClock : TimeProvider, ITimer
    {
        private TimerCallback? _tick;
        private object? _state;
        private long _milliseconds;

        public override long TimestampFrequency => 1000;
        public override long GetTimestamp() => _milliseconds;
        public override ITimer CreateTimer(TimerCallback callback, object? state, TimeSpan dueTime, TimeSpan period)
        {
            _tick = callback;
            _state = state;
            return this;
        }
        public void Step(int milliseconds)
        {
            _milliseconds += milliseconds;
            _tick?.Invoke(_state);
        }
        public bool Change(TimeSpan dueTime, TimeSpan period) => throw new NotSupportedException();
        public void Dispose() => _tick = null;
        public ValueTask DisposeAsync() { Dispose(); return ValueTask.CompletedTask; }
    }
}
