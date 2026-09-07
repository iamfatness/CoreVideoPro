using System.Linq;
using System.Threading.Tasks;
using CoreVideoPro.Control;
using Xunit;

namespace CoreVideoPro.WinUI.Tests;

/// <summary>Plan 7b Task 4 — the panelist board commands. Reuses the rig from
/// <see cref="OhgShowViewModelTests"/> (<c>NewVm</c>, <c>Snapshot</c>, <c>SnapshotA</c>): the same
/// projected-from-JSON slot/panelist shapes, so "occupied vs empty" is read from the real
/// projection, never guessed by the test.</summary>
public sealed partial class OhgShowViewModelTests
{
    [Fact]
    public async Task AssignSelectedToSlot_EmptySlot_SendsPanelistAdd()
    {
        var invoker = new FakeOhgActionInvoker();
        var vm = NewVm(invoker);
        vm.OnSnapshot(Snapshot(SnapshotA));
        vm.SelectedParticipantId = "p9"; // unseated

        // slot 3 is empty in SnapshotA.
        await vm.AssignSelectedToSlotCommand.ExecuteAsync(3);

        var (actionId, args) = Assert.Single(invoker.Invocations);
        Assert.Equal("ohg.panelist.add", actionId);
        Assert.Equal(new object?[] { "p9", 3 }, args);
        Assert.Equal("", vm.LastActionStatus);
    }

    [Fact]
    public async Task AssignSelectedToSlot_OccupiedSlot_SendsPanelistReplace()
    {
        var invoker = new FakeOhgActionInvoker();
        var vm = NewVm(invoker);
        vm.OnSnapshot(Snapshot(SnapshotA));
        vm.SelectedParticipantId = "p9"; // unseated

        // slot 1 is occupied (Ann/p1) in SnapshotA.
        await vm.AssignSelectedToSlotCommand.ExecuteAsync(1);

        var (actionId, args) = Assert.Single(invoker.Invocations);
        Assert.Equal("ohg.panelist.replace", actionId);
        Assert.Equal(new object?[] { 1, "p9" }, args);
    }

    [Fact]
    public async Task AssignSelectedToSlot_NoSelection_RefusesLocally_NeverInvokes()
    {
        var invoker = new FakeOhgActionInvoker();
        var vm = NewVm(invoker);
        vm.OnSnapshot(Snapshot(SnapshotA));

        await vm.AssignSelectedToSlotCommand.ExecuteAsync(3);

        Assert.Empty(invoker.Invocations);
        Assert.Equal("Select a panelist first", vm.LastActionStatus);
    }

    [Fact]
    public async Task AddSelectedToFirstEmpty_SendsPanelistAdd_OneArg()
    {
        var invoker = new FakeOhgActionInvoker();
        var vm = NewVm(invoker);
        vm.OnSnapshot(Snapshot(SnapshotA));
        vm.SelectedParticipantId = "p9";

        await vm.AddSelectedToFirstEmptyCommand.ExecuteAsync(null);

        var (actionId, args) = Assert.Single(invoker.Invocations);
        Assert.Equal("ohg.panelist.add", actionId);
        Assert.Equal(new object?[] { "p9" }, args);
    }

    [Fact]
    public async Task RemoveSlot_SendsPanelistRemove()
    {
        var invoker = new FakeOhgActionInvoker();
        var vm = NewVm(invoker);

        await vm.RemoveSlotCommand.ExecuteAsync(2);

        var (actionId, args) = Assert.Single(invoker.Invocations);
        Assert.Equal("ohg.panelist.remove", actionId);
        Assert.Equal(new object?[] { 2 }, args);
    }

    [Fact]
    public async Task SetRole_SendsPanelistRoleSet()
    {
        var invoker = new FakeOhgActionInvoker();
        var vm = NewVm(invoker);

        await vm.SetRoleCommand.ExecuteAsync(("1001", "host"));

        var (actionId, args) = Assert.Single(invoker.Invocations);
        Assert.Equal("ohg.panelist.role.set", actionId);
        Assert.Equal(new object?[] { "1001", "host" }, args);
    }

    [Theory]
    [InlineData(null)]
    [InlineData("")]
    public async Task SetRole_NoPin_RefusesLocally_NeverInvokes(string? pin)
    {
        var invoker = new FakeOhgActionInvoker();
        var vm = NewVm(invoker);

        await vm.SetRoleCommand.ExecuteAsync((pin!, "host"));

        Assert.Empty(invoker.Invocations);
        Assert.Equal("Panelist has no PIN; set a Mukana override instead", vm.LastActionStatus);
    }

    [Fact]
    public async Task SyncAll_SendsPanelistSyncAll()
    {
        var invoker = new FakeOhgActionInvoker();
        var vm = NewVm(invoker);

        await vm.SyncAllCommand.ExecuteAsync(null);

        var (actionId, args) = Assert.Single(invoker.Invocations);
        Assert.Equal("ohg.panelist.syncAll", actionId);
        Assert.Empty(args);
    }

    [Fact]
    public async Task AFailedInvoke_LandsInStatus_AndRecentRefusals()
    {
        var invoker = new FakeOhgActionInvoker
        {
            Handler = (_, _) => ControlInvokeResult.Fail("manual box fill"),
        };
        var vm = NewVm(invoker);

        await vm.SyncAllCommand.ExecuteAsync(null);

        Assert.Equal("manual box fill", vm.LastActionStatus);
        Assert.Equal("manual box fill", vm.RecentRefusals.FirstOrDefault());
    }

    [Fact]
    public void RolesExposesTheFiveEngineRoles()
    {
        var vm = NewVm();
        Assert.Equal(
            new[] { "panelist", "host", "reader", "aslpanelist", "aslinterpreter" },
            vm.Roles);
    }
}
