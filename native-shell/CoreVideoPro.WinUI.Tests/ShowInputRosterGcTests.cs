using CoreVideoPro.WinUI.Models;
using CoreVideoPro.WinUI.Services;
using Xunit;

namespace CoreVideoPro.WinUI.Tests;

// Lifecycle L7 (source-lifecycle-spec): the persisted roster garbage-collects
// duplicate ghosts so they can never survive two sessions.
public sealed class ShowInputRosterGcTests
{
    private static ShowInputSlotRecord Cap(int slot, string capId, bool inShow) => new()
    {
        SlotNumber = slot,
        Kind = ShowInputKind.UvcWebcam,
        CaptureDeviceId = capId,
        InShow = inShow
    };

    [Fact]
    public void GarbageCollect_DropsParkedDuplicatesButKeepsTheLowestSlot()
    {
        var snapshot = new ShowInputRosterSnapshot
        {
            Version = 1,
            Slots =
            [
                Cap(1, "cam-a", inShow: true),
                Cap(3, "cam-a", inShow: false),
                Cap(7, "cam-a", inShow: false),
                Cap(8, "cam-a", inShow: false),
            ]
        };

        var result = ShowInputRosterSerializer.GarbageCollect(snapshot);

        Assert.Equal(ShowInputRosterSerializer.CurrentVersion, result.Version);
        Assert.Equal(ShowInputKind.UvcWebcam, result.Slots[0].Kind);   // slot 1 kept
        Assert.Equal(ShowInputKind.Unassigned, result.Slots[1].Kind);  // slot 3 ghost cleared
        Assert.Equal(ShowInputKind.Unassigned, result.Slots[2].Kind);  // slot 7 ghost cleared
        Assert.Equal(ShowInputKind.Unassigned, result.Slots[3].Kind);  // slot 8 ghost cleared
        Assert.Null(result.Slots[1].CaptureDeviceId);
    }

    [Fact]
    public void GarbageCollect_KeepsInShowDuplicates()
    {
        // Two in-show slots on the same device is an operator choice (e.g. a
        // wide + a punch-in of one camera): never auto-cleared.
        var snapshot = new ShowInputRosterSnapshot
        {
            Version = 1,
            Slots = [Cap(1, "cam-a", inShow: true), Cap(2, "cam-a", inShow: true)]
        };

        var result = ShowInputRosterSerializer.GarbageCollect(snapshot);

        Assert.Equal(ShowInputKind.UvcWebcam, result.Slots[0].Kind);
        Assert.Equal(ShowInputKind.UvcWebcam, result.Slots[1].Kind);
    }

    [Fact]
    public void GarbageCollect_KeepsDistinctSources()
    {
        var snapshot = new ShowInputRosterSnapshot
        {
            Version = 1,
            Slots = [Cap(1, "cam-a", inShow: false), Cap(2, "cam-b", inShow: false)]
        };

        var result = ShowInputRosterSerializer.GarbageCollect(snapshot);

        Assert.Equal(ShowInputKind.UvcWebcam, result.Slots[0].Kind);
        Assert.Equal(ShowInputKind.UvcWebcam, result.Slots[1].Kind);
    }

    [Fact]
    public void CaptureFrom_PersistsDedupedRosterAtCurrentVersion()
    {
        var slots = new List<ShowInputSlot>();
        for (var i = 1; i <= 4; i++)
        {
            var slot = new ShowInputSlot { SlotNumber = i };
            slot.Kind = ShowInputKind.UvcWebcam;
            slot.CaptureDeviceId = "cam-a";
            slot.InShow = i == 1;  // only slot 1 in show; 2-4 are parked duplicates
            slots.Add(slot);
        }

        var snapshot = ShowInputRosterSerializer.CaptureFrom(slots);

        Assert.Equal(ShowInputRosterSerializer.CurrentVersion, snapshot.Version);
        Assert.Equal(ShowInputKind.UvcWebcam, snapshot.Slots[0].Kind);
        Assert.All(snapshot.Slots.Skip(1), record => Assert.Equal(ShowInputKind.Unassigned, record.Kind));
    }
}
