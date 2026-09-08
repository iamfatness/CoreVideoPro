using System.Collections.Specialized;
using System.Linq;
using System.Threading.Tasks;
using CoreVideoPro.Control;
using Xunit;

namespace CoreVideoPro.WinUI.Tests;

/// <summary>Plan 7b Task 5 — the program panel commands (preview/cut/auto/look/box) and the four
/// computed labels. Reuses the rig from <see cref="OhgShowViewModelTests"/> (<c>NewVm</c>,
/// <c>Snapshot</c>, <c>SnapshotA</c>/<c>SnapshotB</c>) plus a Task-5-specific fixture,
/// <see cref="SnapshotWithLook"/>, that carries a cued look with boxes/page/pageCount, a
/// slot-based preview source, an active speaker, and a non-empty hands queue — the exact shapes
/// the labels need and SnapshotA/B (Task 3's fixtures) deliberately don't carry.</summary>
public sealed partial class OhgShowViewModelTests
{
    /// <summary>Look "wide" cued (page 2 of 3, 0-based wire page=1), box 1 -> slot 2, box 2 blank;
    /// program on the look, preview on slot 4 (Dee); active speaker p1 (Ann); a populated hands
    /// queue with raw (unresolved) ids, matching the brief's own example verbatim.</summary>
    private const string SnapshotWithLook = """
    {
      "revision": 10,
      "slots": [
        {"slot":1,"panelist":{"participantId":"p1","displayName":"Ann","location":"Helsinki","pin":"1001","hasMukana":true,"role":"panelist","online":true,"videoOn":true,"audioOn":true,"handRaised":false}},
        {"slot":2,"panelist":{"participantId":"p2","displayName":"Bob","location":"Turku","role":"host","online":true,"videoOn":false,"audioOn":true,"handRaised":true}},
        {"slot":4,"panelist":{"participantId":"p4","displayName":"Dee","location":"Vaasa","role":"panelist","online":true,"videoOn":true,"audioOn":true,"handRaised":false}}
      ],
      "tally": {"onAirSlots":[1]},
      "panelists": [
        {"participantId":"p1","displayName":"Ann","location":"Helsinki","pin":"1001","hasMukana":true,"role":"panelist","online":true,"videoOn":true,"audioOn":true,"handRaised":false},
        {"participantId":"p2","displayName":"Bob","location":"Turku","role":"host","online":true,"videoOn":false,"audioOn":true,"handRaised":true},
        {"participantId":"p4","displayName":"Dee","location":"Vaasa","role":"panelist","online":true,"videoOn":true,"audioOn":true,"handRaised":false}
      ],
      "unseated": [],
      "gallery": [],
      "queue": {"previous":["0042","0017"],"current":"0099","upcoming":["0003","0120"]},
      "program": {"program":{"kind":"look","lookId":"wide"},"preview":{"kind":"slot","slot":4},"activeSpeakerFollow":false,"activeSpeakerId":"p1"},
      "look": {"lookId":"wide","scenePreset":"panel","hostSlot":1,"readerSlot":null,"boxes":[{"box":1,"slot":2},{"box":2,"slot":0}],"page":1,"pageCount":3,"boxFill":"queue"},
      "smartGallery": false,
      "pagingRefused": null,
      "restoreWarnings": []
    }
    """;

    /// <summary>Same look, one revision later: box 1 re-pointed, box 2 dropped, box 3 added — for
    /// the Boxes no-replace test.</summary>
    private const string SnapshotWithLookMoved = """
    {
      "revision": 11,
      "slots": [
        {"slot":1,"panelist":{"participantId":"p1","displayName":"Ann","location":"Helsinki","pin":"1001","hasMukana":true,"role":"panelist","online":true,"videoOn":true,"audioOn":true,"handRaised":false}},
        {"slot":2,"panelist":{"participantId":"p2","displayName":"Bob","location":"Turku","role":"host","online":true,"videoOn":false,"audioOn":true,"handRaised":true}},
        {"slot":4,"panelist":{"participantId":"p4","displayName":"Dee","location":"Vaasa","role":"panelist","online":true,"videoOn":true,"audioOn":true,"handRaised":false}}
      ],
      "tally": {"onAirSlots":[1]},
      "panelists": [
        {"participantId":"p1","displayName":"Ann","location":"Helsinki","pin":"1001","hasMukana":true,"role":"panelist","online":true,"videoOn":true,"audioOn":true,"handRaised":false},
        {"participantId":"p2","displayName":"Bob","location":"Turku","role":"host","online":true,"videoOn":false,"audioOn":true,"handRaised":true},
        {"participantId":"p4","displayName":"Dee","location":"Vaasa","role":"panelist","online":true,"videoOn":true,"audioOn":true,"handRaised":false}
      ],
      "unseated": [],
      "gallery": [],
      "queue": {"previous":["0042","0017"],"current":"0099","upcoming":["0003","0120"]},
      "program": {"program":{"kind":"look","lookId":"wide"},"preview":{"kind":"slot","slot":4},"activeSpeakerFollow":false,"activeSpeakerId":"p1"},
      "look": {"lookId":"wide","scenePreset":"panel","hostSlot":1,"readerSlot":null,"boxes":[{"box":1,"slot":4},{"box":3,"slot":0}],"page":2,"pageCount":3,"boxFill":"queue"},
      "smartGallery": false,
      "pagingRefused": null,
      "restoreWarnings": []
    }
    """;

    // ── program commands ───────────────────────────────────────────────────────────────

    [Fact]
    public async Task Preview_SendsProgramPreview()
    {
        var invoker = new FakeOhgActionInvoker();
        var vm = NewVm(invoker);

        await vm.PreviewCommand.ExecuteAsync("gallery");

        var (actionId, args) = Assert.Single(invoker.Invocations);
        Assert.Equal("ohg.program.preview", actionId);
        Assert.Equal(new object?[] { "gallery" }, args);
        Assert.Equal("", vm.LastActionStatus);
    }

    [Fact]
    public async Task Cut_SendsProgramCut()
    {
        var invoker = new FakeOhgActionInvoker();
        var vm = NewVm(invoker);

        await vm.CutCommand.ExecuteAsync(null);

        var (actionId, args) = Assert.Single(invoker.Invocations);
        Assert.Equal("ohg.program.cut", actionId);
        Assert.Empty(args);
    }

    [Fact]
    public async Task Auto_SendsProgramAuto()
    {
        var invoker = new FakeOhgActionInvoker();
        var vm = NewVm(invoker);

        await vm.AutoCommand.ExecuteAsync(null);

        var (actionId, args) = Assert.Single(invoker.Invocations);
        Assert.Equal("ohg.program.auto", actionId);
        Assert.Empty(args);
    }

    [Fact]
    public async Task DirectCut_SendsProgramDirectCut()
    {
        var invoker = new FakeOhgActionInvoker();
        var vm = NewVm(invoker);

        await vm.DirectCutCommand.ExecuteAsync("slot:2");

        var (actionId, args) = Assert.Single(invoker.Invocations);
        Assert.Equal("ohg.program.directCut", actionId);
        Assert.Equal(new object?[] { "slot:2" }, args);
    }

    [Theory]
    [InlineData(true)]
    [InlineData(false)]
    public async Task SetAsFollow_SendsProgramAsFollowSet(bool on)
    {
        var invoker = new FakeOhgActionInvoker();
        var vm = NewVm(invoker);

        await vm.SetAsFollowCommand.ExecuteAsync(on);

        var (actionId, args) = Assert.Single(invoker.Invocations);
        Assert.Equal("ohg.program.asFollow.set", actionId);
        Assert.Equal(new object?[] { on }, args);
    }

    [Fact]
    public async Task PreviewSlot_FormatsTheSlotWireStringAndPreviews()
    {
        var invoker = new FakeOhgActionInvoker();
        var vm = NewVm(invoker);

        await vm.PreviewSlotCommand.ExecuteAsync(4);

        var (actionId, args) = Assert.Single(invoker.Invocations);
        Assert.Equal("ohg.program.preview", actionId);
        Assert.Equal(new object?[] { "slot:4" }, args);
    }

    // ── look commands ──────────────────────────────────────────────────────────────────

    [Fact]
    public async Task SetLook_SendsLookSet_AndSetsSelectedLookIdOptimistically()
    {
        var invoker = new FakeOhgActionInvoker();
        var vm = NewVm(invoker);
        Assert.Null(vm.SelectedLookId);

        await vm.SetLookCommand.ExecuteAsync("wide");

        var (actionId, args) = Assert.Single(invoker.Invocations);
        Assert.Equal("ohg.look.set", actionId);
        Assert.Equal(new object?[] { "wide" }, args);
        Assert.Equal("wide", vm.SelectedLookId);
    }

    [Fact]
    public async Task ApplyingASnapshot_OverwritesSelectedLookId_WithTheSnapshotsLook()
    {
        var invoker = new FakeOhgActionInvoker();
        var vm = NewVm(invoker);

        // Optimistic value from an operator pick that has NOT yet been confirmed by a snapshot.
        await vm.SetLookCommand.ExecuteAsync("teatime");
        Assert.Equal("teatime", vm.SelectedLookId);

        vm.OnSnapshot(Snapshot(SnapshotWithLook));
        Assert.Equal("wide", vm.SelectedLookId); // the snapshot's actual look wins

        vm.OnSnapshot(Snapshot(SnapshotA.Replace("\"revision\": 5", "\"revision\": 12")));
        Assert.Null(vm.SelectedLookId); // SnapshotA carries no "look" node at all
    }

    [Fact]
    public async Task NextGuest_SendsLookNextGuest()
    {
        var invoker = new FakeOhgActionInvoker();
        var vm = NewVm(invoker);

        await vm.NextGuestCommand.ExecuteAsync(null);

        var (actionId, args) = Assert.Single(invoker.Invocations);
        Assert.Equal("ohg.look.nextGuest", actionId);
        Assert.Empty(args);
    }

    [Fact]
    public async Task PrevGuest_SendsLookPrevGuest()
    {
        var invoker = new FakeOhgActionInvoker();
        var vm = NewVm(invoker);

        await vm.PrevGuestCommand.ExecuteAsync(null);

        var (actionId, args) = Assert.Single(invoker.Invocations);
        Assert.Equal("ohg.look.prevGuest", actionId);
        Assert.Empty(args);
    }

    [Fact]
    public async Task AssignBox_SendsLookBoxAssign()
    {
        var invoker = new FakeOhgActionInvoker();
        var vm = NewVm(invoker);

        await vm.AssignBoxCommand.ExecuteAsync((2, 3));

        var (actionId, args) = Assert.Single(invoker.Invocations);
        Assert.Equal("ohg.look.box.assign", actionId);
        Assert.Equal(new object?[] { 2, 3 }, args);
    }

    [Fact]
    public async Task ClearBox_SendsLookBoxClear()
    {
        var invoker = new FakeOhgActionInvoker();
        var vm = NewVm(invoker);

        await vm.ClearBoxCommand.ExecuteAsync(2);

        var (actionId, args) = Assert.Single(invoker.Invocations);
        Assert.Equal("ohg.look.box.clear", actionId);
        Assert.Equal(new object?[] { 2 }, args);
    }

    [Fact]
    public async Task AssignSelectedSlotToBox_NoSelection_RefusesLocally_NeverInvokes()
    {
        var invoker = new FakeOhgActionInvoker();
        var vm = NewVm(invoker);

        await vm.AssignSelectedSlotToBoxCommand.ExecuteAsync(1);

        Assert.Empty(invoker.Invocations);
        Assert.Equal("Select a slot first", vm.LastActionStatus);
    }

    [Fact]
    public async Task AssignSelectedSlotToBox_WithSelection_SendsLookBoxAssign()
    {
        var invoker = new FakeOhgActionInvoker();
        var vm = NewVm(invoker);
        vm.SelectedSlot = 3;

        await vm.AssignSelectedSlotToBoxCommand.ExecuteAsync(1);

        var (actionId, args) = Assert.Single(invoker.Invocations);
        Assert.Equal("ohg.look.box.assign", actionId);
        Assert.Equal(new object?[] { 1, 3 }, args);
    }

    // ── Boxes (diff-updated in place) ─────────────────────────────────────────────────

    [Fact]
    public void ApplyingASnapshot_ProjectsBoxes_FromTheCuedLook()
    {
        var vm = NewVm();
        vm.OnSnapshot(Snapshot(SnapshotWithLook));

        Assert.Equal(new[] { 1, 2 }, vm.Boxes.Select(b => b.Box));
        Assert.Equal(2, vm.Boxes[0].Slot);
        Assert.Equal("Bob", vm.Boxes[0].DisplayName);
        Assert.Equal(0, vm.Boxes[1].Slot);
        Assert.Null(vm.Boxes[1].DisplayName);
    }

    [Fact]
    public void NoLook_LeavesBoxesEmpty()
    {
        var vm = NewVm();
        vm.OnSnapshot(Snapshot(SnapshotA));
        Assert.Empty(vm.Boxes);
    }

    [Fact]
    public void BoxesAreUpdatedInPlace_NeverReplaced()
    {
        var vm = NewVm();
        vm.OnSnapshot(Snapshot(SnapshotWithLook));

        var box1 = vm.Boxes.Single(b => b.Box == 1);

        var log = new ActionLog().Watch(vm.Boxes);
        vm.OnSnapshot(Snapshot(SnapshotWithLookMoved));

        Assert.Same(box1, vm.Boxes.Single(b => b.Box == 1));
        Assert.Equal(4, box1.Slot);
        Assert.Equal(new[] { 1, 3 }, vm.Boxes.Select(b => b.Box));
        Assert.DoesNotContain(NotifyCollectionChangedAction.Reset, log.Actions);
    }

    // ── labels ─────────────────────────────────────────────────────────────────────────

    [Fact]
    public void ProgramAndPreviewLabels_BlackAndGallery()
    {
        var vm = NewVm();
        vm.OnSnapshot(Snapshot(SnapshotA));

        Assert.Equal("black", vm.ProgramLabel);
        Assert.Equal("gallery", vm.PreviewLabel);
    }

    [Fact]
    public void ProgramLabel_LookSource_ShowsLabelAndPage()
    {
        var vm = NewVm();
        vm.OnSnapshot(Snapshot(SnapshotWithLook));

        // "wide" is configured with label "Wide" (see NewVm); wire page=1 -> operator page 2/3.
        Assert.Equal("look: Wide (page 2/3)", vm.ProgramLabel);
    }

    [Fact]
    public void PreviewLabel_SlotSource_ShowsSlotAndOccupant()
    {
        var vm = NewVm();
        vm.OnSnapshot(Snapshot(SnapshotWithLook));

        Assert.Equal("slot 4: Dee", vm.PreviewLabel);
    }

    [Fact]
    public void PreviewLabel_SlotSource_EmptySlot_ShowsEMPTY()
    {
        var vm = NewVm();
        vm.OnSnapshot(Snapshot(SnapshotWithLook.Replace("\"slot\":4}", "\"slot\":99}")));

        Assert.Equal("slot 99: EMPTY", vm.PreviewLabel);
    }

    [Fact]
    public void ProgramLabel_ActiveSpeakerSource()
    {
        var vm = NewVm();
        vm.OnSnapshot(Snapshot(SnapshotWithLook.Replace(
            "\"program\":{\"kind\":\"look\",\"lookId\":\"wide\"}",
            "\"program\":{\"kind\":\"activeSpeaker\"}")));

        Assert.Equal("active speaker", vm.ProgramLabel);
    }

    [Fact]
    public void CurrentSpeakerLabel_ResolvesTheActiveSpeakersName()
    {
        var vm = NewVm();
        vm.OnSnapshot(Snapshot(SnapshotWithLook));

        Assert.Equal("Ann", vm.CurrentSpeakerLabel);
    }

    [Fact]
    public void CurrentSpeakerLabel_NoActiveSpeaker_IsEmDash()
    {
        var vm = NewVm();
        vm.OnSnapshot(Snapshot(SnapshotA));

        Assert.Equal("—", vm.CurrentSpeakerLabel);
    }

    [Fact]
    public void QueueLabel_FormatsPreviousCurrentAndNext()
    {
        var vm = NewVm();
        vm.OnSnapshot(Snapshot(SnapshotWithLook));

        Assert.Equal("prev: 0042, 0017 · current: 0099 · next: 0003, 0120", vm.QueueLabel);
    }

    [Fact]
    public void QueueLabel_EmptyQueue_UsesEmDashesForEveryPart()
    {
        var vm = NewVm();
        vm.OnSnapshot(Snapshot(SnapshotA));

        Assert.Equal("prev: — · current: — · next: —", vm.QueueLabel);
    }
}
