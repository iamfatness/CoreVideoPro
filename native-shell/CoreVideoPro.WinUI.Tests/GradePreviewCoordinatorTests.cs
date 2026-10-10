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
        var end = DateTime.UtcNow.AddSeconds(3);
        while (!condition() && DateTime.UtcNow < end) await Task.Delay(10);
        Assert.True(condition());
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
}
