using System.Collections.ObjectModel;
using CoreVideoPro.WinUI.Models;

namespace CoreVideoPro.WinUI.ViewModels.ShowInputs;

/// <summary>
/// The cross-cluster surface <see cref="ShowInputsCoordinator"/> needs from the owning
/// <c>StudioViewModel</c> — the roster/slot model it projects into editors, the room roster and
/// device/media sets the picker options depend on, the readout/reactivity pokes, and the
/// preview/multiview/capture-fleet/media-core refreshes it triggers after an assignment change.
/// StudioViewModel implements this over <c>this</c> (the PR1 MagicScene / PR2 Transport sub-VM
/// pattern), so the bound collections (<c>ShowInputEditors</c> is exposed as a forwarder onto the
/// coordinator's collection; <c>ShowInputs</c>/<c>SrtIngestSources</c> stay [x:Bind] on the god
/// file) keep their exact XAML binding paths.
///
/// Move-only: every member mirrors an existing StudioViewModel property/method; the coordinator
/// just routes the original bodies through it instead of <c>this</c>. Keeping it an interface is
/// what makes the coordinator independently constructible for the characterization tests the god
/// object never allowed.
/// </summary>
public interface IShowInputsHost
{
    // --- roster/slot model + the sets the Source-picker options depend on (read-only) ---
    ObservableCollection<ShowInputSlot> ShowInputs { get; }

    IReadOnlyList<Participant> RoomParticipantsForInputs { get; }

    ObservableCollection<CaptureDevice> CaptureDevices { get; }

    ObservableCollection<AudioCaptureDevice> AudioCaptureDevices { get; }

    IReadOnlyList<MediaBinGroup> MediaBinGroups { get; }

    // --- ISO + auto-assign policy flags (read) ---
    bool IsoRecordingEnabled { get; }

    bool AutomationAutoAssignInputsEnabled { get; }

    // --- SRT ingest sources (stay bound on the god file; the add/remove command bodies move) ---
    ObservableCollection<SrtIngestSource> SrtIngestSources { get; }

    bool CanAddSrtIngestSource { get; }

    int MaxSrtIngestSources { get; }

    SrtIngestSource CreateSrtIngestSource(int number);

    void RemoveVirtualSrtIngestDevice(string deviceId);

    // --- command status line (write only — mirrors the original bodies) ---
    string CommandStatus { set; }

    // --- ShowInputSlotViewModel wiring callbacks (passed into each editor) ---
    void OnShowInputChanged();

    void SetCaptureDeviceAudioSource(string? captureDeviceId, string? audioDeviceId);

    string ResolveSourceDisplayName(string? sourceId, string derivedName);

    void SetSourceDisplayName(string? sourceId, string? name);

    // --- reactivity + readout pokes preserved from the original method bodies ---
    void EnsureAssignedScreensConnected();

    void NotifyShowReadinessChanged();

    void RefreshIsoReadouts();

    /// <summary>Re-raise the ShowInputSummary + MultiviewHeader computed readouts (they stay
    /// bound on StudioViewModel and read the projected editors) — the original
    /// <c>OnPropertyChanged(nameof(ShowInputSummary)); OnPropertyChanged(nameof(MultiviewHeader))</c>
    /// pair inside RefreshShowInputEditors.</summary>
    void RaiseShowInputReadoutsChanged();

    // --- cross-cluster refreshes triggered after an assignment change (stay on the god file) ---
    void RefreshMultiviewGridTiles();

    void RefreshPreviewRoutingState();

    void RefreshDualCaptureSourceOptions();

    void RefreshCaptureFleetSummary();

    Task TrySyncMediaCoreAsync();

    void SaveProductionOutputPreferences();

    /// <summary>UI-thread marshalling seam (forwards to StudioViewModel's private
    /// <c>RunOnUiThread</c>): the take-offline continuation writes bound state and MUST resume on
    /// the UI thread — the 0xc000027b rule, preserved exactly. Tests supply an inline one.</summary>
    void RunOnUiThread(Action action);
}
