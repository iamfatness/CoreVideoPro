using System.Threading.Tasks;
using Xunit;

namespace CoreVideoPro.WinUI.Tests;

/// <summary>Plan 7b Task 6 — the gallery panel commands. Reuses the rig from
/// <see cref="OhgShowViewModelTests"/> (<c>NewVm</c>, <c>Snapshot</c>, <c>SnapshotA</c>).</summary>
public sealed partial class OhgShowViewModelTests
{
    [Fact]
    public async Task ReplaceCellWithSelectedSlot_NoSelection_RefusesLocally_NeverInvokes()
    {
        var invoker = new FakeOhgActionInvoker();
        var vm = NewVm(invoker);

        await vm.ReplaceCellWithSelectedSlotCommand.ExecuteAsync(2);

        Assert.Empty(invoker.Invocations);
        Assert.Equal("Select a slot first", vm.LastActionStatus);
    }

    [Fact]
    public async Task ReplaceCellWithSelectedSlot_SendsGalleryReplace()
    {
        var invoker = new FakeOhgActionInvoker();
        var vm = NewVm(invoker);
        vm.OnSnapshot(Snapshot(SnapshotA));
        vm.SelectedSlot = 1;

        await vm.ReplaceCellWithSelectedSlotCommand.ExecuteAsync(2);

        var (actionId, args) = Assert.Single(invoker.Invocations);
        Assert.Equal("ohg.gallery.replace", actionId);
        Assert.Equal(new object?[] { 2, 1 }, args);
        Assert.Equal("", vm.LastActionStatus);
    }

    [Fact]
    public async Task RemoveCell_SendsGalleryRemove()
    {
        var invoker = new FakeOhgActionInvoker();
        var vm = NewVm(invoker);

        await vm.RemoveCellCommand.ExecuteAsync(3);

        var (actionId, args) = Assert.Single(invoker.Invocations);
        Assert.Equal("ohg.gallery.remove", actionId);
        Assert.Equal(new object?[] { 3 }, args);
    }

    [Fact]
    public async Task ResetGalleryFromSlots_SendsGalleryResetFromSlots()
    {
        var invoker = new FakeOhgActionInvoker();
        var vm = NewVm(invoker);

        await vm.ResetGalleryFromSlotsCommand.ExecuteAsync(null);

        var (actionId, args) = Assert.Single(invoker.Invocations);
        Assert.Equal("ohg.gallery.resetFromSlots", actionId);
        Assert.Empty(args);
    }

    [Fact]
    public async Task EmptyGallery_SendsGalleryEmpty()
    {
        var invoker = new FakeOhgActionInvoker();
        var vm = NewVm(invoker);

        await vm.EmptyGalleryCommand.ExecuteAsync(null);

        var (actionId, args) = Assert.Single(invoker.Invocations);
        Assert.Equal("ohg.gallery.empty", actionId);
        Assert.Empty(args);
    }

    [Theory]
    [InlineData(true)]
    [InlineData(false)]
    public async Task SetSmartGallery_SendsGallerySmartSet(bool on)
    {
        var invoker = new FakeOhgActionInvoker();
        var vm = NewVm(invoker);

        await vm.SetSmartGalleryCommand.ExecuteAsync(on);

        var (actionId, args) = Assert.Single(invoker.Invocations);
        Assert.Equal("ohg.gallery.smart.set", actionId);
        Assert.Equal(new object?[] { on }, args);
    }

    [Fact]
    public void GalleryNoteIsTheFixedCarriedToACoreChangeString()
    {
        var vm = NewVm();
        Assert.Equal("Cell order is not yet applied to Tiles (carried to a core change)", vm.GalleryNote);
    }
}
