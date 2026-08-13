using System.Collections.ObjectModel;
using CoreVideoPro.MediaCore.Services;
using CoreVideoPro.WinUI.Models;
using CoreVideoPro.WinUI.Services;

namespace CoreVideoPro.WinUI.ViewModels.ShowInputs;

/// <summary>
/// Owns the Show-Inputs ROSTER + ASSIGNMENT + AUTO-ASSIGN cluster extracted from the
/// StudioViewModel god object (PR3 of the strangler refactor): the roster persistence, the
/// signature-gated roster → <see cref="ShowInputSlotViewModel"/> projection (the
/// <c>ShowInputEditors</c> the Sources tab binds), the auto-assign of newly-joined Zoom
/// participants to free slots, operator unassign / take-device-offline lifecycle, the SRT-ingest
/// source add/remove, and the per-source ISO selection that feeds recording (the ISO × ShowInputs
/// integration from ISO-4 lives here so it survives roster churn). Move-only, behavior-parity: the
/// method bodies are the originals, with cross-cluster reads/refreshes routed through
/// <see cref="IShowInputsHost"/> instead of <c>this</c>.
///
/// 0xc000027b: <see cref="ShowInputEditors"/> is a signature-gated collection keyed on the
/// participant/device/media id-set (NOT Health — that was the ComboBox-churn crash). The projection
/// diff-updates the editor rows IN PLACE and rebuilds the per-row Source-picker options ONLY when
/// that signature changes — never at snapshot rate. The ISO selection is re-projected under the
/// SAME signature gate. This is preserved exactly.
///
/// This type is independently constructible (a fake <see cref="IShowInputRosterStore"/> + a fake
/// <see cref="IShowInputsHost"/> + a real/fake bridge), which is what carries the first-ever
/// characterization tests of the roster projection + auto-assign + ISO integration.
/// </summary>
public sealed class ShowInputsCoordinator
{
    private readonly IMediaCoreBridge _bridge;
    private readonly IShowInputRosterStore _showInputRosterStore;
    private readonly IShowInputsHost _host;

    private bool _showInputRosterLoaded;
    private string _showInputEditorsSignature = "";

    public ShowInputsCoordinator(IMediaCoreBridge bridge, IShowInputRosterStore rosterStore, IShowInputsHost host)
    {
        _bridge = bridge;
        _showInputRosterStore = rosterStore;
        _host = host;
    }

    /// <summary>The projected editor rows the Sources tab binds (via a same-named forwarder
    /// property on StudioViewModel, so XAML x:Bind is unchanged). One stable instance for the VM's
    /// lifetime — the projection diff-updates it in place; it is never replaced.</summary>
    public ObservableCollection<ShowInputSlotViewModel> ShowInputEditors { get; } = [];

    /// <summary>The operator's per-source ISO selection: canonical scheme-qualified source ids
    /// (<c>zoom:&lt;pid&gt;</c> / <c>capture:&lt;id&gt;</c>). The per-source ISO toggles drive it; it
    /// is re-projected onto the editor rows on every apply. Exposed so StudioViewModel's persistence
    /// (ProductionOutputPreferences v8) can save/restore it.</summary>
    public HashSet<string> IsoSelectedSourceIds { get; } = new(StringComparer.Ordinal);

    // ── roster persistence ────────────────────────────────────────────────────────
    public void LoadShowInputRoster()
    {
        // Persisted state restores ONLY at startup, before the operator can have touched
        // anything. A second load (a future respawn/second-window/deferred path) would
        // overwrite live operator changes with stale disk state — refuse it LOUDLY.
        if (_showInputRosterLoaded)
        {
            LaunchLog.Write("roster: LoadShowInputRoster REFUSED — already loaded this session; " +
                "a re-load would overwrite live operator changes with disk state");
            return;
        }

        try
        {
            var snapshot = _showInputRosterStore.Load();
            // Intent is restored even if the referenced participant/device is no longer present;
            // RefreshShowInputEditors / BuildMultiviewTiles surface unresolved sources as
            // unavailable rather than dropping the assignment.
            ShowInputRosterSerializer.ApplyTo(_host.ShowInputs, snapshot);
        }
        catch (Exception)
        {
            // Persistence is best-effort; never block startup on a bad/locked store.
        }
        finally
        {
            _showInputRosterLoaded = true;
        }
    }

    public void SaveShowInputRoster()
    {
        // Guard against persisting before the saved roster has been loaded/applied.
        if (!_showInputRosterLoaded)
        {
            return;
        }

        try
        {
            _showInputRosterStore.Save(ShowInputRosterSerializer.CaptureFrom(_host.ShowInputs));
        }
        catch (Exception)
        {
            // Best-effort; a failed save must not surface as a user-facing error.
        }
    }

    // ── roster → editor projection ────────────────────────────────────────────────
    public void InitializeShowInputEditors()
    {
        ShowInputEditors.Clear();
        foreach (var slot in _host.ShowInputs)
        {
            ShowInputEditors.Add(new ShowInputSlotViewModel(
                slot,
                OnShowInputEditorChanged,
                _host.SetCaptureDeviceAudioSource,
                _host.ResolveSourceDisplayName,
                _host.SetSourceDisplayName,
                OnShowInputIsoToggled));
        }

        RefreshShowInputEditors(force: true);
    }

    /// <summary>
    /// Every slot change with editors attached lands here (the editor VM raises it on ANY
    /// underlying model change — operator picker, unassign, auto-assign, swaps). Persist
    /// IMMEDIATELY and synchronously: the save used to ride only ApplyShowInputRefresh, a
    /// COALESCED DispatcherQueuePriority.Low dispatcher callback, so an operator change
    /// followed by a crash (four on 2026-08-09 alone) lost the pending save and the
    /// relaunch restored the PRE-change roster over the operator's work. The roster is a
    /// ~2 KB JSON file; a synchronous write per change is nothing.
    /// </summary>
    private void OnShowInputEditorChanged()
    {
        SaveShowInputRoster();
        _host.OnShowInputChanged();
    }

    public void RefreshShowInputEditors(bool force = false)
    {
        _host.EnsureAssignedScreensConnected();
        var mediaAssets = _host.MediaBinGroups.SelectMany(group => group.Assets).ToList();

        // Rebuild each slot's Source dropdown options ONLY when the set the picker
        // depends on actually changes. This method is called from many per-snapshot
        // paths (~10/s); rebuilding the ComboBox ItemsSource every tick resets the
        // operator's in-progress selection (can't pick a participant) and fires a
        // SelectionChanged storm. Skip when nothing changed.
        //
        // The picker membership depends only on WHO is in the room (participant ids) +
        // devices + assets — NOT on participant Health. Health (talking/active-speaker/
        // camera on-off) churns constantly during a live meeting; including it here
        // rebuilt every slot's ItemsSource on every health tick, which blanked the
        // selected Source/name in the picker and flooded the dispatcher (a churn/crash
        // vector). Key the signature on id-set only so options rebuild on join/leave.
        var signature =
            string.Join("|", _host.RoomParticipantsForInputs.Select(p => p.Id)) + "#" +
            string.Join(",", _host.CaptureDevices.Select(d => d.Id)) + "#" +
            _host.AudioCaptureDevices.Count + "#" + mediaAssets.Count;
        if (!force && signature == _showInputEditorsSignature)
        {
            return;
        }
        _showInputEditorsSignature = signature;

        foreach (var editor in ShowInputEditors)
        {
            editor.RefreshSourceOptions(_host.RoomParticipantsForInputs, _host.CaptureDevices, _host.AudioCaptureDevices, mediaAssets);
        }

        // ISO-4: re-project the persisted ISO selection onto the (in-place) editor rows so
        // the per-source ISO toggle reflects the saved selection after roster churn. Gated
        // on the SAME id-set signature above — never a per-tick rebuild (0xc000027b rules).
        ApplyIsoSelectionToEditors();

        _host.RaiseShowInputReadoutsChanged();
        _host.NotifyShowReadinessChanged();
        _host.RefreshIsoReadouts();
    }

    // ── ISO × ShowInputs integration (ISO-4) ──────────────────────────────────────
    /// <summary>Callback from a Sources-tab per-source ISO toggle. Updates the persisted
    /// selection and re-syncs (arming/disarming that ISO writer when recording is live).</summary>
    public void OnShowInputIsoToggled(string? sourceId, bool enabled)
    {
        if (string.IsNullOrWhiteSpace(sourceId))
        {
            return;
        }

        var changed = enabled ? IsoSelectedSourceIds.Add(sourceId) : IsoSelectedSourceIds.Remove(sourceId);
        if (!changed)
        {
            return;
        }

        _host.SaveProductionOutputPreferences();
        _host.RefreshIsoReadouts();
        _ = _host.TrySyncMediaCoreAsync();
    }

    /// <summary>Re-project the persisted ISO selection + eligibility onto the live editor
    /// rows (in place — never a collection rebuild; obeys the 0xc000027b rules).</summary>
    public void ApplyIsoSelectionToEditors()
    {
        foreach (var editor in ShowInputEditors)
        {
            editor.SetIsoSelected(editor.SourceId is { Length: > 0 } id && IsoSelectedSourceIds.Contains(id));
        }
    }

    /// <summary>
    /// ISO-4 (spec §7): resolve the operator's ISO recording selection into the canonical
    /// scheme-qualified `isoSourceIds` (plus the legacy bare-id projection). Gated by the
    /// "Program + ISOs" master switch — OFF returns EMPTY so no ISO writers arm and program
    /// recording is byte-identical to the program-only path. Pure logic lives in the
    /// unit-tested <see cref="IsoSourceSelectionResolver"/>.
    /// </summary>
    public (IReadOnlyList<string> SourceIds, IReadOnlyList<string> ParticipantIds) BuildIsoSourceTargets()
    {
        var sourceIds = IsoSourceSelectionResolver.Resolve(
            _host.IsoRecordingEnabled,
            IsoSelectedSourceIds,
            ComputeEligiblePresentIsoSourceIds());
        var participantIds = IsoSourceSelectionResolver.ToLegacyParticipantIds(sourceIds);
        return (sourceIds, participantIds);
    }

    /// <summary>
    /// The currently ISO-eligible + present video-bearing source ids (`zoom:&lt;pid&gt;` /
    /// `capture:&lt;id&gt;`), in roster order. Only assigned, non-missing Zoom guests and
    /// capture devices are eligible (media playout is a non-goal). A selected source that
    /// has since left / unplugged is excluded here, so no dangling ISO writer arms.
    /// </summary>
    public IReadOnlyList<string> ComputeEligiblePresentIsoSourceIds()
    {
        var list = new List<string>();
        foreach (var editor in ShowInputEditors)
        {
            if (editor.ShowIsoToggle && !editor.IsSourceMissing && editor.SourceId is { Length: > 0 } id)
            {
                list.Add(id);
            }
        }

        return list;
    }

    // ── auto-assign (roster → free slots) ─────────────────────────────────────────
    // Participant ids this coordinator has already offered to auto-assign in the
    // current meeting. Auto-assign only places ids it has NEVER seen — i.e. real
    // newcomers. Without this memory it refilled every "not currently assigned"
    // id on every sync tick (~1/s), so an operator's manual unassign/replace on
    // the Sources screen was reverted within a second. Cleared when the operator
    // explicitly re-runs auto-assign (ReapplyShowInputAutoAssign) — that request
    // MEANS "assign everyone".
    private readonly HashSet<string> _autoAssignSeenParticipantIds = new(StringComparer.Ordinal);

    public void SyncShowInputsFromMeeting(
        IReadOnlyList<LiveProductionSync.LiveProductionParticipantContext> participants)
    {
        // Free slots whose participant left, and (when auto-assign is on) fill FREE slots
        // with NEWLY-JOINED participants only — never someone the operator removed.
        var rosterIds = participants
            .Select(participant => participant.Id)
            .Where(id => !string.IsNullOrWhiteSpace(id))
            .ToList();
        var newcomers = rosterIds
            .Where(id => !_autoAssignSeenParticipantIds.Contains(id))
            .ToList();
        // Leavers drop out of the set so a genuine leave-and-rejoin (same id,
        // e.g. breakout return) is a newcomer again.
        _autoAssignSeenParticipantIds.Clear();
        _autoAssignSeenParticipantIds.UnionWith(rosterIds);

        ShowInputRosterService.SyncZoomParticipantSlots(
            _host.ShowInputs,
            rosterIds,
            _host.AutomationAutoAssignInputsEnabled,
            newcomers);

        RefreshShowInputEditors();
        _host.RefreshMultiviewGridTiles();
    }

    // Re-run the roster→slot auto-assign from the CURRENT room roster (used when the operator
    // flips the auto-assign toggle). No-op stale-removal when there is no meeting.
    // MUST use RoomParticipantsForInputs (all in-room, video on OR off) — the SAME set the
    // meeting-sync path passes. RoomVideoParticipants filters out camera-off participants, so
    // using it here would make the helper's stale-removal pass free a camera-off participant's
    // assigned slot the moment the operator flips the toggle.
    public void ReapplyShowInputAutoAssign()
    {
        // The operator flipped the toggle: that is an explicit "assign everyone
        // now", so forget which ids were seen and fill from the whole roster
        // (candidates omitted = every unassigned id is eligible).
        _autoAssignSeenParticipantIds.Clear();
        ShowInputRosterService.SyncZoomParticipantSlots(
            _host.ShowInputs,
            _host.RoomParticipantsForInputs
                .Select(participant => participant.Id)
                .Where(id => !string.IsNullOrWhiteSpace(id))
                .ToList(),
            _host.AutomationAutoAssignInputsEnabled);

        RefreshShowInputEditors();
        _host.RefreshMultiviewGridTiles();
    }

    // ── lifecycle: take offline / unassign ────────────────────────────────────────
    // Lifecycle L2: take a device offline - stop its core session (WGC/SRT),
    // unassign every slot that references it, refresh. Webcams using the WinUI
    // MediaCapture bridge keep their existing capture-toggle path; this covers
    // the core-session kinds (screens today).
    public async Task TakeCaptureDeviceOfflineAsync(string deviceId)
    {
        if (string.IsNullOrWhiteSpace(deviceId))
        {
            return;
        }
        LaunchLog.Write(string.Format("lifecycle: take offline {0}", deviceId));
        try
        {
            var statuses = await _bridge.DisconnectNativeCaptureDeviceAsync(deviceId).ConfigureAwait(false);
            var match = statuses.FirstOrDefault(s => s.Id == deviceId);
            _host.RunOnUiThread(() =>
            {
                var device = _host.CaptureDevices.FirstOrDefault(d => d.Id == deviceId);
                if (device is not null)
                {
                    device.ConnectionState = CaptureConnectionState.Detected;
                    device.SignalPresent = match?.SignalPresent ?? false;
                }
                foreach (var editor in ShowInputEditors.Where(e => string.Equals(e.CaptureDeviceId, deviceId, StringComparison.Ordinal)).ToList())
                {
                    LaunchLog.Write(string.Format("lifecycle: offline {0} unassigns slot {1}", deviceId, editor.SlotNumber));
                    editor.Unassign();
                }
                RefreshShowInputEditors(force: true);
                _host.RefreshDualCaptureSourceOptions();
                _host.RefreshCaptureFleetSummary();
                _host.RefreshPreviewRoutingState();
                _host.RefreshMultiviewGridTiles();
                _host.CommandStatus = string.Format("{0} is offline.", device?.Name ?? deviceId);
                _ = _host.TrySyncMediaCoreAsync();
            });
        }
        catch (Exception ex)
        {
            LaunchLog.Write(string.Format("lifecycle: take offline failed {0}: {1}", deviceId, ex.Message));
        }
    }

    // Lifecycle L1: operator-facing unassign (button on each Show Input row).
    public void UnassignShowInput(ShowInputSlotViewModel editor)
    {
        if (editor is null)
        {
            return;
        }
        LaunchLog.Write(string.Format("lifecycle: unassign slot {0} (was {1} cap={2} pid={3})",
            editor.SlotNumber, editor.Kind, editor.CaptureDeviceId ?? "-", editor.ParticipantId ?? "-"));
        editor.Unassign();
        RefreshShowInputEditors(force: true);
        _host.RefreshPreviewRoutingState();
        _host.RefreshMultiviewGridTiles();
        _ = _host.TrySyncMediaCoreAsync();
    }

    // ── SRT ingest sources (routable virtual inputs) ──────────────────────────────
    public void AddSrtIngestSource()
    {
        if (!_host.CanAddSrtIngestSource)
        {
            _host.CommandStatus = $"SRT ingest is capped at {_host.MaxSrtIngestSources} sources.";
            return;
        }

        var nextNumber = Enumerable.Range(1, _host.MaxSrtIngestSources)
            .First(number => _host.SrtIngestSources.All(source => source.Number != number));
        var source = _host.CreateSrtIngestSource(nextNumber);
        _host.SrtIngestSources.Add(source);
        _host.CommandStatus = $"{source.Name} added as a routable SRT input";
    }

    public void RemoveSrtIngestSource(string sourceId)
    {
        if (_host.SrtIngestSources.Count <= 1)
        {
            _host.CommandStatus = "Keep at least one SRT ingest source configured.";
            return;
        }

        var source = _host.SrtIngestSources.FirstOrDefault(item => string.Equals(item.Id, sourceId, StringComparison.Ordinal));
        if (source is null)
        {
            return;
        }

        _host.SrtIngestSources.Remove(source);
        _host.RemoveVirtualSrtIngestDevice(source.DeviceId);
        _host.CommandStatus = $"{source.Name} removed from SRT inputs";
    }
}
