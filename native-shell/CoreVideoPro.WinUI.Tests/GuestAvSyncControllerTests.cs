using CoreVideoPro.MediaCore.Models;
using CoreVideoPro.WinUI.ViewModels;
using Xunit;

namespace CoreVideoPro.WinUI.Tests;

public sealed class GuestAvSyncControllerTests
{
    [Fact]
    public async Task SelectedGuestNudgesAreIsolatedClampedAndReset()
    {
        string? selected = "101";
        var sends = new List<(string Id, int Offset)>();
        long revision = 0;
        var controller = new GuestAvSyncController((id, offset) =>
        {
            sends.Add((id, offset));
            return Task.FromResult(new NativeZoomGuestAvSyncAck
            {
                OffsetMs = offset, Revision = ++revision
            });
        }, () => selected, () => true);

        await controller.NudgeEarlierAsync();
        Assert.Equal(-10, controller.OffsetMs);
        Assert.Contains("Video 10 ms", controller.Label);
        selected = "202";
        controller.Refresh();
        Assert.Equal(0, controller.OffsetMs);
        await controller.SetAsync(250);
        Assert.Equal(200, controller.OffsetMs);
        Assert.Contains("Audio +200 ms", controller.Label);
        selected = "101";
        controller.Refresh();
        Assert.Equal(-10, controller.OffsetMs);
        await controller.ResetAsync();
        Assert.Equal(0, controller.OffsetMs);
        Assert.Equal(new[] { ("101", -10), ("202", 200), ("101", 0) }, sends);
    }

    [Fact]
    public async Task ProgramMixCannotPretendToApplyGuestTrimAndSnapshotRepairsState()
    {
        var mode = false;
        var sends = 0;
        var controller = new GuestAvSyncController((_, offset) =>
        {
            sends++;
            return Task.FromResult(new NativeZoomGuestAvSyncAck
            {
                OffsetMs = offset, Revision = sends + 1
            });
        }, () => "101", () => mode);
        await controller.SetAsync(80);
        Assert.Equal(0, sends);
        Assert.Contains("program mix", controller.Explanation);

        mode = true;
        controller.Refresh();
        controller.ApplySnapshot(1, [new NativeZoomGuestAvSyncSetting
        {
            ParticipantId = "101", OffsetMs = -70
        }]);
        Assert.Equal(-70, controller.OffsetMs);
        await controller.NudgeLaterAsync();
        Assert.Equal(-60, controller.OffsetMs);
        controller.ApplySnapshot(1, [new NativeZoomGuestAvSyncSetting
        {
            ParticipantId = "101", OffsetMs = -70
        }]);
        Assert.Equal(-60, controller.OffsetMs); // older fact cannot roll back ack
        controller.ClearSession();
        Assert.Equal(0, controller.OffsetMs);
    }
}
