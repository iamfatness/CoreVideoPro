using System;
using System.Collections.Generic;
using System.Collections.ObjectModel;
using System.Collections.Specialized;
using System.Linq;
using System.Text.Json;
using System.Threading.Tasks;
using CoreVideoPro.Control;
using CoreVideoPro.ShowEngine;
using CoreVideoPro.WinUI.Services;
using CoreVideoPro.WinUI.ViewModels;
using Xunit;

namespace CoreVideoPro.WinUI.Tests;

/// <summary>Plan 7b Task 3 — the workspace VM's threading-sensitive heart: marshaled ingestion,
/// diff-updated keyed collections (0xc000027b: never <c>Clear()+Add</c> a bound collection), and
/// the status strip.
///
/// The VM is constructed with plain data and NO <c>DispatcherQueue</c> — the marshal delegate is
/// <c>a =&gt; a()</c> — so every fact here is provable off the UI thread, the same convention
/// every other VM test in this assembly uses.</summary>
public sealed partial class OhgShowViewModelTests
{
    // ── fixtures ──────────────────────────────────────────────────────────────────────

    private static ShowEngineHealth Stopped => new(ShowEngineState.Stopped, 0, 0, null, null);

    private static JsonElement Json(string json) => JsonDocument.Parse(json).RootElement.Clone();

    private static ShowEngineSnapshot Snapshot(string json, int generation = 1)
    {
        var element = Json(json);
        var revision = element.TryGetProperty("revision", out var r) && r.ValueKind == JsonValueKind.Number
            ? r.GetInt64()
            : 0L;
        return new ShowEngineSnapshot(generation, revision, element, new Dictionary<string, JsonElement>());
    }

    /// <summary>Two seated panelists (slot 1 Ann, slot 2 Bob), slot 1 on air, one unseated guest,
    /// a two-cell gallery and one restore warning.</summary>
    private const string SnapshotA = """
    {
      "revision": 5,
      "slots": [
        {"slot":1,"panelist":{"participantId":"p1","displayName":"Ann","location":"Helsinki","pin":"1001","hasMukana":true,"role":"panelist","online":true,"videoOn":true,"audioOn":true,"handRaised":false}},
        {"slot":2,"panelist":{"participantId":"p2","displayName":"Bob","location":"Turku","role":"host","online":true,"videoOn":false,"audioOn":true,"handRaised":true}},
        {"slot":3,"panelist":null}
      ],
      "tally": {"onAirSlots":[1]},
      "panelists": [
        {"participantId":"p1","displayName":"Ann","location":"Helsinki","pin":"1001","hasMukana":true,"role":"panelist","online":true,"videoOn":true,"audioOn":true,"handRaised":false},
        {"participantId":"p2","displayName":"Bob","location":"Turku","role":"host","online":true,"videoOn":false,"audioOn":true,"handRaised":true}
      ],
      "unseated": [
        {"participantId":"p9","displayName":"Zed","location":"Oulu","role":"panelist","online":true}
      ],
      "gallery": [
        {"cell":1,"slot":1},
        {"cell":2,"slot":0}
      ],
      "queue": {"previous":[],"current":null,"upcoming":[]},
      "program": {"program":{"kind":"black"},"preview":{"kind":"gallery"},"activeSpeakerFollow":false},
      "smartGallery": false,
      "pagingRefused": null,
      "restoreWarnings": ["look 'wide' had no boxes"]
    }
    """;

    /// <summary>Same shape at revision 6: Ann renamed + off air, Bob DEPARTED, a new panelist
    /// "p0" (sorted BEFORE the survivors by slot/id), the gallery re-pointed and paging refused.</summary>
    private const string SnapshotB = """
    {
      "revision": 6,
      "slots": [
        {"slot":1,"panelist":{"participantId":"p1","displayName":"Ann Virtanen","location":"Espoo","pin":"1001","hasMukana":true,"role":"panelist","online":true,"videoOn":false,"audioOn":false,"handRaised":true}},
        {"slot":2,"panelist":{"participantId":"p0","displayName":"Cid","location":"Vaasa","role":"panelist","online":true,"videoOn":true,"audioOn":true,"handRaised":false}},
        {"slot":3,"panelist":null}
      ],
      "tally": {"onAirSlots":[2]},
      "panelists": [
        {"participantId":"p0","displayName":"Cid","location":"Vaasa","role":"panelist","online":true,"videoOn":true,"audioOn":true,"handRaised":false},
        {"participantId":"p1","displayName":"Ann Virtanen","location":"Espoo","pin":"1001","hasMukana":true,"role":"panelist","online":true,"videoOn":false,"audioOn":false,"handRaised":true}
      ],
      "unseated": [],
      "gallery": [
        {"cell":1,"slot":2},
        {"cell":2,"slot":1}
      ],
      "queue": {"previous":[],"current":null,"upcoming":[]},
      "program": {"program":{"kind":"gallery"},"preview":{"kind":"black"},"activeSpeakerFollow":true},
      "smartGallery": true,
      "pagingRefused": "no next page",
      "restoreWarnings": []
    }
    """;

    private static OhgShowViewModel NewVm(
        FakeOhgActionInvoker? invoker = null,
        Action<Action>? marshal = null,
        Func<ShowEngineSnapshot?>? latest = null,
        Func<ShowEngineHealth>? health = null,
        bool driveHost = true)
        => new(
            invoker ?? new FakeOhgActionInvoker(),
            marshal ?? (a => a()),
            latest ?? (() => null),
            health ?? (() => Stopped),
            new[] { new OhgLookOption("wide", "Wide") },
            driveHost);

    /// <summary>Records every <see cref="NotifyCollectionChangedAction"/> raised on a collection so
    /// a test can prove no <c>Reset</c> (the <c>Clear()+Add</c> signature) ever occurred.</summary>
    private sealed class ActionLog
    {
        public List<NotifyCollectionChangedAction> Actions { get; } = new();

        public ActionLog Watch<T>(ObservableCollection<T> collection)
        {
            collection.CollectionChanged += (_, e) => Actions.Add(e.Action);
            return this;
        }
    }

    // ── the eight facts ───────────────────────────────────────────────────────────────

    [Fact]
    public void ApplyingASnapshot_FillsEveryCollection_WithoutReplacingThem()
    {
        var vm = NewVm();

        var panelists = vm.Panelists;
        var slots = vm.Slots;
        var gallery = vm.Gallery;
        var unseated = vm.Unseated;
        var warnings = vm.RestoreWarnings;

        var log = new ActionLog();
        log.Watch(panelists).Watch(slots).Watch(gallery).Watch(unseated).Watch(warnings);

        vm.OnSnapshot(Snapshot(SnapshotA));

        Assert.Equal(new[] { "p1", "p2" }, vm.Panelists.Select(p => p.ParticipantId));
        Assert.Equal(new[] { 1, 2, 3 }, vm.Slots.Select(s => s.Slot));
        Assert.Equal(new[] { 1, 2 }, vm.Gallery.Select(c => c.Cell));
        Assert.Equal(new[] { "p9" }, vm.Unseated.Select(p => p.ParticipantId));
        Assert.Equal(new[] { "look 'wide' had no boxes" }, vm.RestoreWarnings);
        Assert.Equal(5L, vm.Revision);
        Assert.True(vm.Slots[0].OnAir);
        Assert.False(vm.Slots[1].OnAir);
        Assert.Equal("Ann", vm.Slots[0].DisplayName);
        Assert.NotNull(vm.Current);

        // A second, DIFFERENT snapshot must still mutate the same instances.
        vm.OnSnapshot(Snapshot(SnapshotB));

        Assert.Same(panelists, vm.Panelists);
        Assert.Same(slots, vm.Slots);
        Assert.Same(gallery, vm.Gallery);
        Assert.Same(unseated, vm.Unseated);
        Assert.Same(warnings, vm.RestoreWarnings);
        Assert.DoesNotContain(NotifyCollectionChangedAction.Reset, log.Actions);
    }

    [Fact]
    public void ASecondSnapshotWithTheSameRevision_IsANoOp()
    {
        var vm = NewVm();
        vm.OnSnapshot(Snapshot(SnapshotA));

        var log = new ActionLog().Watch(vm.Panelists).Watch(vm.Slots).Watch(vm.Gallery).Watch(vm.Unseated);
        var ann = vm.Panelists[0];
        var propertyChanges = 0;
        ann.PropertyChanged += (_, _) => propertyChanges++;

        // Same revision, but the payload claims a different name: the gate must refuse it.
        vm.OnSnapshot(Snapshot(SnapshotA.Replace("\"Ann\"", "\"Not Ann\"")));

        Assert.Equal("Ann", vm.Panelists[0].DisplayName);
        Assert.Empty(log.Actions);
        Assert.Equal(0, propertyChanges);
    }

    [Fact]
    public void AFirstSnapshotOfANewGeneration_IsApplied_EvenWithTheSameRevision()
    {
        var vm = NewVm();
        vm.OnSnapshot(Snapshot(SnapshotA, generation: 1));

        // A respawned engine (generation 2) that happens to restart at the same revision number
        // must NOT be swallowed by the revision-only gate — the payload differs and must land.
        vm.OnSnapshot(Snapshot(SnapshotA.Replace("\"Ann\"", "\"Not Ann\""), generation: 2));

        Assert.Equal("Not Ann", vm.Panelists[0].DisplayName);
    }

    [Fact]
    public void SelectingAParticipant_SetsExactlyThatRowsFlag_AndClearsThePrevious()
    {
        var vm = NewVm();
        vm.OnSnapshot(Snapshot(SnapshotA));

        var ann = vm.Panelists.Single(p => p.ParticipantId == "p1");
        var bob = vm.Panelists.Single(p => p.ParticipantId == "p2");
        var zed = vm.Unseated.Single(p => p.ParticipantId == "p9");

        vm.SelectedParticipantId = "p1";
        Assert.True(ann.IsSelected);
        Assert.False(bob.IsSelected);
        Assert.False(zed.IsSelected);

        vm.SelectedParticipantId = "p9";
        Assert.False(ann.IsSelected);
        Assert.False(bob.IsSelected);
        Assert.True(zed.IsSelected);

        vm.SelectedParticipantId = null;
        Assert.False(ann.IsSelected);
        Assert.False(zed.IsSelected);
    }

    [Fact]
    public void SelectingASlot_SetsExactlyThatRowsFlag_AndClearsThePrevious()
    {
        var vm = NewVm();
        vm.OnSnapshot(Snapshot(SnapshotA));

        var slot1 = vm.Slots.Single(s => s.Slot == 1);
        var slot2 = vm.Slots.Single(s => s.Slot == 2);

        vm.SelectedSlot = 1;
        Assert.True(slot1.IsSelected);
        Assert.False(slot2.IsSelected);

        vm.SelectedSlot = 2;
        Assert.False(slot1.IsSelected);
        Assert.True(slot2.IsSelected);
    }

    [Fact]
    public void RowsAreUpdatedInPlace_NewRowsInsertedSorted_DepartedRemoved()
    {
        var vm = NewVm();
        vm.OnSnapshot(Snapshot(SnapshotA));

        var ann = vm.Panelists.Single(p => p.ParticipantId == "p1");
        var slot1 = vm.Slots.Single(s => s.Slot == 1);
        var cell1 = vm.Gallery.Single(c => c.Cell == 1);

        vm.OnSnapshot(Snapshot(SnapshotB));

        // survivors: SAME instances, new values
        Assert.Same(ann, vm.Panelists.Single(p => p.ParticipantId == "p1"));
        Assert.Equal("Ann Virtanen", ann.DisplayName);
        Assert.Equal("Espoo", ann.Location);
        Assert.True(ann.HandRaised);

        Assert.Same(slot1, vm.Slots.Single(s => s.Slot == 1));
        Assert.False(slot1.OnAir);
        Assert.Equal("Ann Virtanen", slot1.DisplayName);

        Assert.Same(cell1, vm.Gallery.Single(c => c.Cell == 1));
        Assert.Equal(2, cell1.Slot);

        // newcomer inserted at its sorted position; departed removed
        Assert.Equal(new[] { "p0", "p1" }, vm.Panelists.Select(p => p.ParticipantId));
        Assert.DoesNotContain(vm.Panelists, p => p.ParticipantId == "p2");
        Assert.Empty(vm.Unseated);
    }

    [Fact]
    public void SelectionSurvivesASnapshot()
    {
        var vm = NewVm();
        vm.OnSnapshot(Snapshot(SnapshotA));

        vm.SelectedParticipantId = "p1";
        vm.SelectedSlot = 2;

        vm.OnSnapshot(Snapshot(SnapshotB));

        Assert.Equal("p1", vm.SelectedParticipantId);
        Assert.Equal((int?)2, vm.SelectedSlot);

        // …but a selection whose key is gone is cleared rather than left dangling.
        vm.SelectedParticipantId = "p2";
        vm.OnSnapshot(Snapshot(SnapshotB.Replace("\"revision\": 6", "\"revision\": 7")));
        Assert.Null(vm.SelectedParticipantId);
        Assert.Equal((int?)2, vm.SelectedSlot);
    }

    [Fact]
    public void HealthProjectsToStateAndDetail()
    {
        var vm = NewVm();

        vm.OnHealth(new ShowEngineHealth(ShowEngineState.Failed, 4, 5, "spawn failed: ENOENT", DateTimeOffset.UnixEpoch));
        Assert.Equal("failed", vm.EngineState);
        Assert.Equal("spawn failed: ENOENT", vm.EngineDetail);

        vm.OnHealth(new ShowEngineHealth(ShowEngineState.Running, 3, 1, null, null));
        Assert.Equal("running", vm.EngineState);
        Assert.Equal("gen 3, 1 restarts", vm.EngineDetail);
    }

    [Fact]
    public void WarnAndErrorLogsFeedTheRefusalStrip_NewestFirst_CappedAtTen()
    {
        var vm = NewVm();

        vm.OnLog(new ShowEngineLogLine("info", "engine ready"));
        Assert.Empty(vm.RecentRefusals);

        for (var i = 1; i <= 12; i++)
        {
            vm.OnLog(new ShowEngineLogLine(i % 2 == 0 ? "error" : "warn", $"refusal {i}"));
        }

        Assert.Equal(10, vm.RecentRefusals.Count);
        Assert.Contains("refusal 12", vm.RecentRefusals[0]);
        Assert.Contains("refusal 3", vm.RecentRefusals[9]);
        Assert.DoesNotContain(vm.RecentRefusals, line => line.Contains("engine ready"));
    }

    [Fact]
    public void MarshalIsUsedForEveryIngestPath()
    {
        var marshalled = 0;
        var vm = NewVm(marshal: a => { marshalled++; a(); });

        var baseline = marshalled;   // construction may marshal its initial apply
        vm.OnSnapshot(Snapshot(SnapshotA));
        Assert.True(marshalled > baseline, "OnSnapshot must go through the marshal");

        var afterSnapshot = marshalled;
        vm.OnHealth(new ShowEngineHealth(ShowEngineState.Running, 1, 0, null, null));
        Assert.True(marshalled > afterSnapshot, "OnHealth must go through the marshal");

        var afterHealth = marshalled;
        vm.OnLog(new ShowEngineLogLine("warn", "refused"));
        Assert.True(marshalled > afterHealth, "OnLog must go through the marshal");
    }

    [Fact]
    public async Task RestartEngine_InvokesTheInvoker()
    {
        var invoker = new FakeOhgActionInvoker();
        var vm = NewVm(invoker);

        await vm.RestartEngineCommand.ExecuteAsync(null);
        Assert.Equal(1, invoker.RestartCalls);
        Assert.Equal("", vm.LastActionStatus);

        invoker.RestartResult = ControlInvokeResult.Fail("engine gave up");
        await vm.RestartEngineCommand.ExecuteAsync(null);
        Assert.Equal(2, invoker.RestartCalls);
        Assert.Equal("engine gave up", vm.LastActionStatus);
    }

    // ── construction + status strip scalars ───────────────────────────────────────────

    [Fact]
    public void ConstructionAppliesTheLatestSnapshotAndHealth()
    {
        var snapshot = Snapshot(SnapshotA);
        var vm = NewVm(
            latest: () => snapshot,
            health: () => new ShowEngineHealth(ShowEngineState.Running, 2, 0, null, null));

        Assert.Equal(5L, vm.Revision);
        Assert.Equal(2, vm.Panelists.Count);
        Assert.Equal("running", vm.EngineState);
        Assert.Equal("gen 2, 0 restarts", vm.EngineDetail);
    }

    [Fact]
    public void ShadowModeIsTheInverseOfDriveHost()
    {
        Assert.True(NewVm(driveHost: false).IsShadowMode);
        Assert.False(NewVm(driveHost: true).IsShadowMode);

        var vm = NewVm(driveHost: false);
        Assert.Equal("", vm.ShadowLastCommand);
        vm.SetShadowLastCommand("cueLook wide");
        Assert.Equal("cueLook wide", vm.ShadowLastCommand);
    }

    [Fact]
    public void PagingRefusedIsEmptyWhenNull()
    {
        var vm = NewVm();
        vm.OnSnapshot(Snapshot(SnapshotA));
        Assert.Equal("", vm.PagingRefused);

        vm.OnSnapshot(Snapshot(SnapshotB));
        Assert.Equal("no next page", vm.PagingRefused);
    }
}
