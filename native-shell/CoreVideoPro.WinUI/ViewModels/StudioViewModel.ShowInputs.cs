using CoreVideoPro.MediaCore.Services;
using CoreVideoPro.WinUI.Models;
using CoreVideoPro.WinUI.ViewModels.ShowInputs;

namespace CoreVideoPro.WinUI.ViewModels;

/// <summary>
/// StudioViewModel's Show-Inputs façade (PR3 strangler). The roster persistence + roster→editor
/// projection + auto-assign + unassign / take-offline lifecycle + SRT-ingest add/remove + the
/// per-source ISO selection all live in <see cref="ShowInputsCoordinator"/>; StudioViewModel keeps
/// the bound <c>ShowInputEditors</c> collection (a forwarder onto the coordinator's stable
/// instance, so XAML x:Bind is unchanged) and the generated <c>[RelayCommand]</c> objects
/// (UnassignShowInputCommand, TakeCaptureDeviceOfflineCommand, AddSrtIngestSourceCommand,
/// RemoveSrtIngestSourceCommand), forwarding their bodies here. StudioViewModel implements
/// <see cref="IShowInputsHost"/> over <c>this</c> (the PR1 MagicScene / PR2 Transport sub-VM
/// pattern) — the bound roster state (ShowInputs, SrtIngestSources, the ISO readouts) stays on the
/// god file; the coordinator reads/refreshes it through the interface.
///
/// The high-fan-in projection helpers (RefreshShowInputEditors is called from ~20 snapshot-apply
/// sites, BuildIsoSourceTargets from the recording payload builders) keep same-named private
/// forwarders so those call sites are untouched — the lowest-churn, highest-parity shape.
/// </summary>
public sealed partial class StudioViewModel : IShowInputsHost
{
    // _showInputsCoordinator is declared + constructed in StudioViewModel.cs (it needs `this` as
    // its IShowInputsHost, so it is assigned in the ctor before the first roster load).

    // ── same-named private forwarders (call sites across StudioViewModel are unchanged) ──
    private void LoadShowInputRoster() => _showInputsCoordinator.LoadShowInputRoster();

    private void SaveShowInputRoster() => _showInputsCoordinator.SaveShowInputRoster();

    private void InitializeShowInputEditors() => _showInputsCoordinator.InitializeShowInputEditors();

    private void RefreshShowInputEditors(bool force = false) => _showInputsCoordinator.RefreshShowInputEditors(force);

    private void ApplyIsoSelectionToEditors() => _showInputsCoordinator.ApplyIsoSelectionToEditors();

    private (IReadOnlyList<string> SourceIds, IReadOnlyList<string> ParticipantIds) BuildIsoSourceTargets() =>
        _showInputsCoordinator.BuildIsoSourceTargets();

    private IReadOnlyList<string> ComputeEligiblePresentIsoSourceIds() =>
        _showInputsCoordinator.ComputeEligiblePresentIsoSourceIds();

    private void SyncShowInputsFromMeeting(
        IReadOnlyList<LiveProductionSync.LiveProductionParticipantContext> participants) =>
        _showInputsCoordinator.SyncShowInputsFromMeeting(participants);

    private void ReapplyShowInputAutoAssign() => _showInputsCoordinator.ReapplyShowInputAutoAssign();

    // ── IShowInputsHost: members the coordinator calls that are private/renamed on the god file.
    //    (Public matching members — ShowInputs, CaptureDevices, AudioCaptureDevices,
    //    SrtIngestSources, RoomParticipantsForInputs, MediaBinGroups, IsoRecordingEnabled,
    //    AutomationAutoAssignInputsEnabled, CanAddSrtIngestSource, CommandStatus,
    //    SetCaptureDeviceAudioSource, ResolveSourceDisplayName, SetSourceDisplayName — implicitly
    //    implement the interface, so they are not re-declared here.) ──
    int IShowInputsHost.MaxSrtIngestSources => MaxSrtIngestSources;

    SrtIngestSource IShowInputsHost.CreateSrtIngestSource(int number) => CreateSrtIngestSource(number);

    void IShowInputsHost.RemoveVirtualSrtIngestDevice(string deviceId) => RemoveVirtualSrtIngestDevice(deviceId);

    void IShowInputsHost.OnShowInputChanged() => OnShowInputChanged();

    void IShowInputsHost.EnsureAssignedScreensConnected() => EnsureAssignedScreensConnected();

    void IShowInputsHost.NotifyShowReadinessChanged() => NotifyShowReadinessChanged();

    void IShowInputsHost.RefreshIsoReadouts() => RefreshIsoReadouts();

    void IShowInputsHost.RaiseShowInputReadoutsChanged()
    {
        OnPropertyChanged(nameof(ShowInputSummary));
        OnPropertyChanged(nameof(MultiviewHeader));
    }

    void IShowInputsHost.RefreshMultiviewGridTiles() => RefreshMultiviewGridTiles();

    void IShowInputsHost.RefreshPreviewRoutingState() => RefreshPreviewRoutingState();

    void IShowInputsHost.RefreshDualCaptureSourceOptions() => RefreshDualCaptureSourceOptions();

    void IShowInputsHost.RefreshCaptureFleetSummary() => RefreshCaptureFleetSummary();

    Task IShowInputsHost.TrySyncMediaCoreAsync() => TrySyncMediaCoreAsync();

    void IShowInputsHost.SaveProductionOutputPreferences() => SaveProductionOutputPreferences();

    void IShowInputsHost.RunOnUiThread(Action action) => RunOnUiThread(action);
}
