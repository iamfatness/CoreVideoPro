using System.Collections.ObjectModel;
using CoreVideoPro.MediaCore.Models;
using CoreVideoPro.MediaCore.Services;
using CoreVideoPro.WinUI.Models;
using CoreVideoPro.WinUI.Services;
using CoreVideoPro.WinUI.ViewModels;
using CoreVideoPro.WinUI.ViewModels.ShowInputs;
using Xunit;

namespace CoreVideoPro.WinUI.Tests;

/// <summary>
/// Characterization tests for <see cref="ShowInputsCoordinator"/> — the ShowInputs roster /
/// assignment / auto-assign / ISO-selection cluster extracted from the StudioViewModel god object
/// (PR3 strangler). These are the FIRST-EVER coverage of the roster→editor projection: they exist
/// because the coordinator is constructible in isolation (a real <see cref="InMemoryShowInputRosterStore"/>
/// + a fake <see cref="IShowInputsHost"/> + a fake <see cref="IMediaCoreBridge"/>), which
/// StudioViewModel itself is not. They lock the golden-path behavior — the 0xc000027b signature
/// gate, auto-assign of free slots, operator unassign, and the ISO × ShowInputs integration
/// (ISO-4) surviving a roster refresh — BEFORE anyone refactors it further.
/// </summary>
public sealed class ShowInputsCoordinatorTests
{
    private static (ShowInputsCoordinator Coordinator, FakeShowInputsHost Host) Build()
    {
        var host = new FakeShowInputsHost();
        var coordinator = new ShowInputsCoordinator(new FakeMediaCoreBridge(), new InMemoryShowInputRosterStore(), host);
        coordinator.InitializeShowInputEditors();
        return (coordinator, host);
    }

    // ---------------------------------------------------------------- signature gating

    [Fact]
    public void RefreshEditors_SameIdSet_SkipsRebuild_ChangedIdSet_DiffUpdatesInPlace()
    {
        var (coordinator, host) = Build();
        var editorsInstance = coordinator.ShowInputEditors;
        var slotCount = editorsInstance.Count;
        Assert.Equal(host.ShowInputs.Count, slotCount);

        // Initialize already forced one projection. A no-op refresh with the SAME id-set must skip
        // past the signature gate (no readout/readiness pokes) — the 0xc000027b "never rebuild a
        // bound collection at snapshot rate" rule.
        var readinessBefore = host.NotifyShowReadinessCount;
        coordinator.RefreshShowInputEditors();
        Assert.Equal(readinessBefore, host.NotifyShowReadinessCount);

        // Change the id-set the picker depends on → the signature changes → the projection runs
        // (readiness poke fires), diff-updating the SAME collection instance IN PLACE (never
        // replaced; count stable).
        host.RoomParticipantsForInputs = [new Participant { Id = "p1", Name = "Guest 1" }];
        coordinator.RefreshShowInputEditors();
        Assert.Equal(readinessBefore + 1, host.NotifyShowReadinessCount);
        Assert.Same(editorsInstance, coordinator.ShowInputEditors);
        Assert.Equal(slotCount, coordinator.ShowInputEditors.Count);

        // A second refresh with that same (now-current) id-set skips again.
        coordinator.RefreshShowInputEditors();
        Assert.Equal(readinessBefore + 1, host.NotifyShowReadinessCount);
    }

    // ---------------------------------------------------------------- auto-assign

    [Fact]
    public void ReapplyAutoAssign_FillsAFreeSlot_WithARoomParticipant_WhenEnabled()
    {
        var (coordinator, host) = Build();
        host.AutomationAutoAssignInputsEnabled = true;
        host.RoomParticipantsForInputs = [new Participant { Id = "p1", Name = "Guest 1" }];

        coordinator.ReapplyShowInputAutoAssign();

        var assigned = host.ShowInputs.FirstOrDefault(slot => slot.Kind == ShowInputKind.ZoomParticipant);
        Assert.NotNull(assigned);
        Assert.Equal("p1", assigned!.ParticipantId);
        Assert.True(assigned.InShow);
    }

    [Fact]
    public void ReapplyAutoAssign_Disabled_DoesNotAssignFreeSlots()
    {
        var (coordinator, host) = Build();
        host.AutomationAutoAssignInputsEnabled = false;
        host.RoomParticipantsForInputs = [new Participant { Id = "p1", Name = "Guest 1" }];

        coordinator.ReapplyShowInputAutoAssign();

        Assert.DoesNotContain(host.ShowInputs, slot => slot.Kind == ShowInputKind.ZoomParticipant);
    }

    // ---------------------------------------------------------------- unassign

    [Fact]
    public void UnassignShowInput_ClearsTheSlot()
    {
        var (coordinator, host) = Build();
        host.AutomationAutoAssignInputsEnabled = true;
        host.RoomParticipantsForInputs = [new Participant { Id = "p1", Name = "Guest 1" }];
        coordinator.ReapplyShowInputAutoAssign();

        var editor = coordinator.ShowInputEditors.First(e => e.Kind == ShowInputKind.ZoomParticipant);
        Assert.True(editor.IsAssigned);

        coordinator.UnassignShowInput(editor);

        Assert.False(editor.IsAssigned);
        Assert.Equal(ShowInputKind.Unassigned, editor.Kind);
        Assert.DoesNotContain(host.ShowInputs, slot => slot.Kind == ShowInputKind.ZoomParticipant);
    }

    // ---------------------------------------------------------------- ISO × ShowInputs integration

    [Fact]
    public void IsoSelection_SurvivesARosterRefresh_ViaInPlaceReProjection()
    {
        var (coordinator, host) = Build();
        host.AutomationAutoAssignInputsEnabled = true;
        host.RoomParticipantsForInputs = [new Participant { Id = "p1", Name = "Guest 1" }];
        coordinator.ReapplyShowInputAutoAssign();

        var editor = coordinator.ShowInputEditors.First(e => e.Kind == ShowInputKind.ZoomParticipant);
        Assert.True(editor.ShowIsoToggle);
        var sourceId = editor.SourceId;
        Assert.False(string.IsNullOrEmpty(sourceId));

        // Operator toggles ISO on for this source (the TwoWay-bound checkbox → the editor's
        // IsoEnabled setter fires the coordinator's OnShowInputIsoToggled callback, which records
        // the persisted selection).
        editor.IsoEnabled = true;
        Assert.Contains(sourceId!, coordinator.IsoSelectedSourceIds);
        Assert.True(editor.IsoEnabled);

        // Simulate a row losing its ISO flag (as a wholesale rebuild would). The signature-gated
        // roster refresh re-projects the PERSISTED selection back onto the row IN PLACE
        // (ApplyIsoSelectionToEditors) — the ISO × ShowInputs integration must survive roster
        // churn so the isoSourceIds recording selection is never silently dropped.
        editor.SetIsoSelected(false);
        Assert.False(editor.IsoEnabled);

        // A second guest joins → the participant id-set signature changes → the projection runs.
        // p1 is still present, so its row keeps its source (not source-missing).
        host.RoomParticipantsForInputs =
        [
            new Participant { Id = "p1", Name = "Guest 1" },
            new Participant { Id = "p2", Name = "Guest 2" }
        ];
        coordinator.RefreshShowInputEditors();

        Assert.True(editor.IsoEnabled); // re-projected from the persisted selection
        Assert.Contains(sourceId!, coordinator.IsoSelectedSourceIds);

        // And the eligible-present projection that feeds recording still resolves it.
        Assert.Contains(sourceId!, coordinator.ComputeEligiblePresentIsoSourceIds());
    }

    [Fact]
    public void BuildIsoSourceTargets_Empty_WhenMasterSwitchOff()
    {
        var (coordinator, host) = Build();
        host.AutomationAutoAssignInputsEnabled = true;
        host.RoomParticipantsForInputs = [new Participant { Id = "p1", Name = "Guest 1" }];
        coordinator.ReapplyShowInputAutoAssign();
        var editor = coordinator.ShowInputEditors.First(e => e.Kind == ShowInputKind.ZoomParticipant);
        editor.IsoEnabled = true;

        host.IsoRecordingEnabled = false;
        Assert.Empty(coordinator.BuildIsoSourceTargets().SourceIds);

        host.IsoRecordingEnabled = true;
        // needs the eligible-present projection refreshed so the row is not source-missing
        coordinator.RefreshShowInputEditors(force: true);
        Assert.Contains(editor.SourceId!, coordinator.BuildIsoSourceTargets().SourceIds);
    }

    // ---------------------------------------------------------------- fakes

    private sealed class FakeShowInputsHost : IShowInputsHost
    {
        public ObservableCollection<ShowInputSlot> ShowInputs { get; } =
            new(ShowInputRosterService.CreateDefaultSlots());

        public IReadOnlyList<Participant> RoomParticipantsForInputs { get; set; } = [];

        public ObservableCollection<CaptureDevice> CaptureDevices { get; } = [];

        public ObservableCollection<AudioCaptureDevice> AudioCaptureDevices { get; } = [];

        public IReadOnlyList<MediaBinGroup> MediaBinGroups { get; set; } = [];

        public bool IsoRecordingEnabled { get; set; }

        public bool AutomationAutoAssignInputsEnabled { get; set; }

        public ObservableCollection<SrtIngestSource> SrtIngestSources { get; } = [];

        public bool CanAddSrtIngestSource => SrtIngestSources.Count < MaxSrtIngestSources;

        public int MaxSrtIngestSources => 8;

        public int NotifyShowReadinessCount { get; private set; }

        public string? LastCommandStatus { get; private set; }

        public SrtIngestSource CreateSrtIngestSource(int number) =>
            new() { Id = $"srt-source-{number:00}", Number = number };

        public void RemoveVirtualSrtIngestDevice(string deviceId) { }

        public string CommandStatus { set => LastCommandStatus = value; }

        public void OnShowInputChanged() { }

        public void SetCaptureDeviceAudioSource(string? captureDeviceId, string? audioDeviceId) { }

        public string ResolveSourceDisplayName(string? sourceId, string derivedName) => derivedName;

        public void SetSourceDisplayName(string? sourceId, string? name) { }

        public void EnsureAssignedScreensConnected() { }

        public void NotifyShowReadinessChanged() => NotifyShowReadinessCount++;

        public void RefreshIsoReadouts() { }

        public void RaiseShowInputReadoutsChanged() { }

        public void RefreshMultiviewGridTiles() { }

        public void RefreshPreviewRoutingState() { }

        public void RefreshDualCaptureSourceOptions() { }

        public void RefreshCaptureFleetSummary() { }

        public Task TrySyncMediaCoreAsync() => Task.CompletedTask;

        public void SaveProductionOutputPreferences() { }

        public void RunOnUiThread(Action action) => action();
    }

    private sealed class FakeMediaCoreBridge : IMediaCoreBridge
    {
        public bool Running => true;

        public NativeMediaCoreProfile? Profile => null;

        public string ProfileSummary => "GPU 1080p60";

        public NativeMediaCoreStateSnapshot? LastSnapshot => null;

#pragma warning disable CS0067 // events are part of the seam surface; the coordinator does not raise them
        public event Action<MediaCoreHealth>? HealthChanged;
        public event Action<string>? StatusChanged;
        public event Action<NativeMediaCoreProfile>? ProfileChanged;
        public event Action<NativeMediaCoreStateSnapshot>? SnapshotChanged;
        public event Action<ZoomVideoFrame>? ZoomVideoFrameReceived;
        public event Action<ProgramFramePreview>? ProgramFramePreviewReceived;
        public event Action<ProgramSharedTexture>? ProgramSharedTextureReceived;
        public event Action<ProgramSharedTexture>? PreviewSharedTextureReceived;
        public event Action<ParticipantSharedTexture>? ParticipantSharedTextureReceived;
        public event Action<MultiviewSharedTexture>? MultiviewSharedTextureReceived;
#pragma warning restore CS0067

        public void ConfigureZoomSpineSync(Func<Dictionary<string, object?>>? payloadFactory) { }

        public Task<NativeMediaCoreProfile?> StartAsync(CancellationToken cancellationToken = default) =>
            Task.FromResult<NativeMediaCoreProfile?>(null);

        public void Stop() { }

        public Task<NativeMediaCoreStateSnapshot> PollSnapshotAsync(CancellationToken cancellationToken = default) =>
            Task.FromResult(new NativeMediaCoreStateSnapshot());

        public Task<NativeMediaCoreStateSnapshot> SyncAsync(
            IReadOnlyList<NativeMediaCoreCommand> commands, double? elapsedMs = null,
            CancellationToken cancellationToken = default) => throw new NotSupportedException();

        public Task<RawCaptureSnapshot> StopZoomCaptureAsync(CancellationToken cancellationToken = default) =>
            throw new NotSupportedException();

        public Task<RawCaptureSnapshot> GetZoomSnapshotAsync(CancellationToken cancellationToken = default) =>
            throw new NotSupportedException();

        public Task OpenVstEditorAsync(string selection, CancellationToken cancellationToken = default) =>
            throw new NotSupportedException();

        public Task SetVstParamAsync(string selection, long paramId, double normalized,
            CancellationToken cancellationToken = default) => throw new NotSupportedException();

        public Task SetVstStateAsync(string selection, string stateBase64,
            CancellationToken cancellationToken = default) => throw new NotSupportedException();

        public Task<string?> GetVstStateAsync(string selection, CancellationToken cancellationToken = default) =>
            throw new NotSupportedException();

        public Task SetCaptureAudioSyncOffsetAsync(string deviceId, int offsetMs,
            CancellationToken cancellationToken = default) => throw new NotSupportedException();

        public Task RegisterCaptureShmAsync(string deviceId, string shmName, int width, int height,
            CancellationToken cancellationToken = default) => throw new NotSupportedException();

        public Task<IReadOnlyList<NativeCaptureDeviceStatus>> ConnectNativeCaptureDeviceAsync(
            string deviceId, CancellationToken cancellationToken = default, string? outputSourceId = null) =>
            throw new NotSupportedException();

        public Task<IReadOnlyList<NativeCaptureDeviceStatus>> ListNativeCaptureDevicesAsync(
            CancellationToken cancellationToken = default) => throw new NotSupportedException();

        public Task<IReadOnlyList<NativeCaptureDeviceStatus>> DisconnectNativeCaptureDeviceAsync(
            string deviceId, CancellationToken cancellationToken = default) =>
            Task.FromResult<IReadOnlyList<NativeCaptureDeviceStatus>>([]);

        public Task AddBrowserSourceAsync(string url, int width, int height, int fps,
            CancellationToken cancellationToken = default) => throw new NotSupportedException();

        public Task RemoveBrowserSourceAsync(string browserId, CancellationToken cancellationToken = default) =>
            throw new NotSupportedException();

        public Task ReloadBrowserSourceAsync(string browserId, CancellationToken cancellationToken = default) =>
            throw new NotSupportedException();

        public ValueTask DisposeAsync() => ValueTask.CompletedTask;
    }
}
