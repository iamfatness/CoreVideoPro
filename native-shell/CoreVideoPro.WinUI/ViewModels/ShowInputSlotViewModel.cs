using CoreVideoPro.WinUI.Models;
using CoreVideoPro.WinUI.Services;
using System.ComponentModel;

namespace CoreVideoPro.WinUI.ViewModels;

public sealed class ShowInputSlotViewModel : INotifyPropertyChanged
{
    private readonly ShowInputSlot _slot;
    private readonly Action _onChanged;
    private readonly Action<string?, string?> _onAudioDeviceChanged;
    // (canonical source id, derived name) -> effective display name (override or derived).
    private readonly Func<string?, string, string> _resolveDisplayName;
    // (canonical source id, new name | null-to-reset) -> persist the override.
    private readonly Action<string?, string?> _setDisplayName;
    // ISO-4: (canonical source id, enabled) -> update the operator's ISO selection.
    private readonly Action<string?, bool> _onIsoToggled;
    private IReadOnlyList<Participant> _participants = [];
    private IReadOnlyList<CaptureDevice> _captureDevices = [];
    private IReadOnlyList<AudioCaptureDevice> _audioDevices = [];
    private IReadOnlyList<MediaAsset> _mediaAssets = [];
    private bool _suppressChangedCallback;
    private bool _isoEnabled;
    private bool _suppressIsoCallback;

    public ShowInputSlotViewModel(
        ShowInputSlot slot,
        Action onChanged,
        Action<string?, string?>? onAudioDeviceChanged = null,
        Func<string?, string, string>? resolveDisplayName = null,
        Action<string?, string?>? setDisplayName = null,
        Action<string?, bool>? onIsoToggled = null)
    {
        _slot = slot;
        _onChanged = onChanged;
        _onAudioDeviceChanged = onAudioDeviceChanged ?? ((_, _) => { });
        _resolveDisplayName = resolveDisplayName ?? ((_, derived) => derived);
        _setDisplayName = setDisplayName ?? ((_, _) => { });
        _onIsoToggled = onIsoToggled ?? ((_, _) => { });
        _slot.PropertyChanged += (_, _) =>
        {
            // Re-raise the VM's bound properties on ANY underlying model change. The roster
            // restore (ShowInputRosterStore.ApplyTo) and slot swaps mutate the MODEL
            // directly; without this, a row realized before the restore kept rendering the
            // default empty slot ("Select a source" + a greyed stale checkbox) until some
            // later refresh happened to run — the intermittent stale row the old auto-pick
            // used to paper over.
            OnSlotPropertyChanged();
            if (!_suppressChangedCallback)
            {
                _onChanged();
            }
        };
    }

    public int SlotNumber => _slot.SlotNumber;

    // Lifecycle L1 (source-lifecycle-spec 3.2): detach the slot entirely -
    // Kind=Unassigned nulls the ids (model OnKindChanged) and the slot leaves
    // the show. Idempotent.
    public void Unassign()
    {
        using var _ = ShowInputWriteScope.Enter("operator-unassign");
        _slot.InShow = false;
        _slot.Kind = ShowInputKind.Unassigned;
        _onChanged();
    }

    public string SlotLabel => _slot.SlotLabel;

    public ShowInputKind Kind
    {
        get => _slot.Kind;
        set
        {
            if (_slot.Kind == value)
            {
                return;
            }

            using (ShowInputWriteScope.Enter("operator-picker"))
            {
                _slot.Kind = value;
            }
            // Pass ALL the retained lists. This previously passed only participants +
            // capture devices, so the optional audioDevices/mediaAssets params defaulted
            // to [] -- switching a slot's TYPE to "Media asset" wiped the media list (an
            // empty source dropdown that still DISPLAYED the previous device's text) and
            // dropped the audio-device options. (owner: "trying to add a media input but
            // none are showing and it has a webcam name", 2026-07-11)
            RefreshSourceOptions(_participants, _captureDevices, _audioDevices, _mediaAssets);
            OnSlotPropertyChanged();
        }
    }

    public string? ParticipantId
    {
        get => _slot.ParticipantId;
        set
        {
            if (_slot.ParticipantId == value)
            {
                return;
            }

            using (ShowInputWriteScope.Enter("operator-picker"))
            {
                _slot.ParticipantId = value;
            }
            OnSlotPropertyChanged();
        }
    }

    public string? CaptureDeviceId
    {
        get => _slot.CaptureDeviceId;
        set
        {
            if (_slot.CaptureDeviceId == value)
            {
                return;
            }

            using (ShowInputWriteScope.Enter("operator-picker"))
            {
                _slot.CaptureDeviceId = value;
            }
            SyncAudioDeviceFromCaptureDevice();
            OnSlotPropertyChanged();
        }
    }

    public string? AudioDeviceId
    {
        get => _slot.AudioDeviceId;
        set
        {
            if (_slot.AudioDeviceId == value)
            {
                return;
            }

            _slot.AudioDeviceId = string.IsNullOrWhiteSpace(value) ? null : value;
            _onAudioDeviceChanged(CaptureDeviceId, _slot.AudioDeviceId);
            OnSlotPropertyChanged();
        }
    }

    public bool InShow
    {
        get => _slot.InShow;
        set
        {
            if (_slot.InShow == value)
            {
                return;
            }

            using (ShowInputWriteScope.Enter("operator-inshow"))
            {
                _slot.InShow = value;
            }
            OnSlotPropertyChanged();
        }
    }

    public bool IsSourcePickerEnabled => _slot.IsSourcePickerEnabled;

    public bool ShowInShowToggle => true;

    /// <summary>ISO-4: this source is eligible for per-source ISO recording — an assigned,
    /// video-bearing Zoom guest or capture device (media playout is a non-goal). Drives
    /// visibility of the per-row "ISO" toggle.</summary>
    public bool ShowIsoToggle => IsAssigned && Kind switch
    {
        ShowInputKind.ZoomParticipant or ShowInputKind.Blackmagic or ShowInputKind.Aja or
        ShowInputKind.UvcWebcam or ShowInputKind.Screen or ShowInputKind.SrtIngest or
        ShowInputKind.Browser => true,
        _ => false
    };

    /// <summary>ISO-4: whether this source is selected for ISO recording. TwoWay-bound to the
    /// per-row "ISO" checkbox; toggling raises the ISO callback so the StudioViewModel updates
    /// (and persists) the operator's ISO selection. Re-projected in place from the persisted
    /// selection via <see cref="SetIsoSelected"/> on every roster apply.</summary>
    public bool IsoEnabled
    {
        get => _isoEnabled;
        set
        {
            if (_isoEnabled == value)
            {
                return;
            }

            _isoEnabled = value;
            OnPropertyChanged(nameof(IsoEnabled));
            if (!_suppressIsoCallback)
            {
                _onIsoToggled(SourceId, value);
            }
        }
    }

    /// <summary>Set the ISO-selected state WITHOUT firing the toggle callback (used when the
    /// StudioViewModel re-projects the persisted selection onto the row).</summary>
    public void SetIsoSelected(bool enabled)
    {
        if (_isoEnabled == enabled)
        {
            return;
        }

        _suppressIsoCallback = true;
        try
        {
            _isoEnabled = enabled;
            OnPropertyChanged(nameof(IsoEnabled));
        }
        finally
        {
            _suppressIsoCallback = false;
        }
    }

    public bool ShowAudioDevicePicker => _slot.IsAudioPickerEnabled;

    public string? SelectedSourceId
    {
        get => Kind is ShowInputKind.ZoomParticipant or ShowInputKind.Media ? ParticipantId : CaptureDeviceId;
        set
        {
            if (Kind is ShowInputKind.ZoomParticipant or ShowInputKind.Media)
            {
                ParticipantId = value;
            }
            else
            {
                CaptureDeviceId = value;
            }
        }
    }

    /// <summary>The canonical source id for this slot's assigned source (zoom:/capture:/media:),
    /// or null when unassigned. Key for the display-name override.</summary>
    public string? SourceId => ShowInputRosterService.SlotSourceId(_slot);

    /// <summary>SRC-1: single unified picker — get is the canonical source id; set parses the
    /// canonical id, infers the Kind (zoom:/media:/capture:-with-device-lookup) and assigns
    /// kind + id together, so the operator never touches a TYPE dropdown. Hint rows and
    /// unknown values are ignored.</summary>
    public string? SelectedUnifiedSourceId
    {
        get => SourceId;
        set
        {
            if (value is null ||
                ShowInputRosterService.IsHintSourceId(value) ||
                string.Equals(SourceId, value, StringComparison.Ordinal))
            {
                // Snap the ComboBox back onto the model value (hint rows are informational).
                OnPropertyChanged(nameof(SelectedUnifiedSourceId));
        OnPropertyChanged(nameof(SelectedUnifiedSourceLabel));
                return;
            }

            if (value.StartsWith(ShowInputRosterService.ZoomSourcePrefix, StringComparison.Ordinal))
            {
                using var _ = ShowInputWriteScope.Enter("operator-picker");
                _slot.Kind = ShowInputKind.ZoomParticipant;
                _slot.ParticipantId = value[ShowInputRosterService.ZoomSourcePrefix.Length..];
            }
            else if (value.StartsWith(ShowInputRosterService.MediaSourcePrefix, StringComparison.Ordinal))
            {
                using var _ = ShowInputWriteScope.Enter("operator-picker");
                _slot.Kind = ShowInputKind.Media;
                // Media slots store the full "media:<assetId>" id in ParticipantId.
                _slot.ParticipantId = value;
            }
            else if (value.StartsWith(ShowInputRosterService.CaptureSourcePrefix, StringComparison.Ordinal))
            {
                var deviceId = value[ShowInputRosterService.CaptureSourcePrefix.Length..];
                var device = _captureDevices.FirstOrDefault(item =>
                    string.Equals(item.Id, deviceId, StringComparison.Ordinal));
                using (ShowInputWriteScope.Enter("operator-picker"))
                {
                    _slot.Kind = device is null
                        ? ShowInputKind.UvcWebcam
                        : ShowInputRosterService.InferCaptureDeviceKind(device);
                    _slot.CaptureDeviceId = deviceId;
                }
                SyncAudioDeviceFromCaptureDevice();
            }
            else
            {
                OnPropertyChanged(nameof(SelectedUnifiedSourceId));
        OnPropertyChanged(nameof(SelectedUnifiedSourceLabel));
                return;
            }

            RefreshSourceOptions(_participants, _captureDevices, _audioDevices, _mediaAssets);
            OnSlotPropertyChanged();
        }
    }

    public IReadOnlyList<ShowInputSourceOption> UnifiedSourceOptions { get; private set; } = [];

    /// <summary>Button-face label for the picker. An unavailable saved binding is presented
    /// as an inactive saved source while the slot is out of show, and as an actionable offline
    /// state only while the slot is enabled.</summary>
    public string SelectedUnifiedSourceLabel
    {
        get
        {
            if (_isSourceMissing)
            {
                return InShow
                    ? "Source offline — choose or reconnect"
                    : "Not in show · saved source";
            }

            return ShowInputRosterService.UnifiedSourceDisplayLabel(UnifiedSourceOptions, SourceId) ?? "Select a source";
        }
    }

    private bool _isSourceMissing;

    /// <summary>True when the slot is assigned but its source is no longer available
    /// (device unplugged, guest left, asset removed). The binding is KEPT — the source
    /// re-attaches when it returns; we never silently substitute another source.</summary>
    public bool IsSourceMissing => _isSourceMissing;

    /// <summary>An unavailable source is an operator warning only when the slot is enabled.
    /// Disabled slots retain their binding quietly so they can reconnect later.</summary>
    public bool ShowSourceUnavailableWarning => InShow && _isSourceMissing;

    /// <summary>Assigned slots that are out of show are visibly passive without disabling
    /// their picker or Unassign action.</summary>
    public double RowOpacity => IsAssigned && !InShow ? 0.72 : 1.0;

    public string KindLabel => _slot.KindLabel;

    /// <summary>Assigned rows carry the full control set (name, audio, grade, unassign);
    /// unassigned rows collapse to just the picker — visual hierarchy, not a wall of
    /// dead controls.</summary>
    public bool IsAssigned => _slot.IsAssigned;

    /// <summary>Editable display name for the assigned source. Defaults to the derived
    /// Zoom/UVC/asset name; setting it stores the operator override (feeding the auto
    /// lower-thirds and multiview labels). Setting it blank resets to the derived name.</summary>
    public string DisplayName
    {
        get => _resolveDisplayName(SourceId, DerivedSourceName());
        set
        {
            var derived = DerivedSourceName();
            var trimmed = value?.Trim() ?? string.Empty;
            // Persist blank (or "same as derived") as a reset so we never store a redundant override.
            _setDisplayName(SourceId, string.Equals(trimmed, derived, StringComparison.Ordinal) ? null : trimmed);
            OnPropertyChanged(nameof(DisplayName));
        }
    }

    public bool IsDisplayNameEditable => !string.IsNullOrEmpty(SourceId);

    private string DerivedSourceName() => Kind switch
    {
        ShowInputKind.ZoomParticipant when ParticipantId is { Length: > 0 } pid =>
            _participants.FirstOrDefault(p => string.Equals(p.Id, pid, StringComparison.Ordinal))?.Name ?? string.Empty,
        ShowInputKind.Media when ShowInputRosterService.TryGetMediaAssetId(ParticipantId, out var assetId) =>
            _mediaAssets.FirstOrDefault(a => string.Equals(a.Id, assetId, StringComparison.Ordinal))?.Name ?? string.Empty,
        ShowInputKind.Blackmagic or ShowInputKind.Aja or ShowInputKind.UvcWebcam or ShowInputKind.Screen or ShowInputKind.SrtIngest or ShowInputKind.Browser
            when CaptureDeviceId is { Length: > 0 } deviceId =>
            _captureDevices.FirstOrDefault(d => string.Equals(d.Id, deviceId, StringComparison.Ordinal))?.Name ?? string.Empty,
        _ => string.Empty
    };

    public string? SourceColorGradeId =>
        Kind is ShowInputKind.ZoomParticipant or ShowInputKind.Media
            ? ParticipantId
            : string.IsNullOrWhiteSpace(CaptureDeviceId) ? null : $"capture:{CaptureDeviceId}";

    public IReadOnlyList<ShowInputKindOption> KindOptions { get; } = ShowInputRosterService.KindOptions;

    public IReadOnlyList<ShowInputSourceOption> SourceOptions { get; private set; } = [];

    public IReadOnlyList<ShowInputSourceOption> AudioDeviceOptions { get; private set; } = [];

    public void RefreshSourceOptions(
        IReadOnlyList<Participant> participants,
        IReadOnlyList<CaptureDevice> captureDevices,
        IReadOnlyList<AudioCaptureDevice>? audioDevices = null,
        IReadOnlyList<MediaAsset>? mediaAssets = null)
    {
        _participants = participants;
        _captureDevices = captureDevices;
        _audioDevices = audioDevices ?? [];
        _mediaAssets = mediaAssets ?? [];
        _suppressChangedCallback = true;
        try
        {
            SourceOptions = ShowInputRosterService.BuildSourceOptions(Kind, participants, captureDevices, _mediaAssets);
            AudioDeviceOptions = ShowInputRosterService.BuildAudioSourceOptions(_audioDevices);

            // SRC-1: ONE grouped picker; the slot's kind follows the picked source. NO silent
            // auto-pick: the old fallback here substituted the FIRST available source whenever
            // the current one wasn't in the rebuilt list, so a device blip (or the old
            // type-switch flow) could re-point a slot at something the operator never chose —
            // mid-show. A gone source now remains as a saved-unavailable entry and the binding
            // is kept so it re-attaches when the source returns.
            var unified = ShowInputRosterService.BuildUnifiedSourceOptions(participants, captureDevices, _mediaAssets);
            _isSourceMissing = SourceId is { Length: > 0 } assignedId &&
                !unified.Any(option => string.Equals(option.Value, assignedId, StringComparison.Ordinal));
            UnifiedSourceOptions = _isSourceMissing
                ? ShowInputRosterService.BuildUnifiedSourceOptions(
                    participants, captureDevices, _mediaAssets,
                    SourceId, _resolveDisplayName(SourceId, DerivedSourceName()))
                : unified;

            SyncAudioDeviceFromCaptureDevice();
            if (!ShowAudioDevicePicker ||
                (!string.IsNullOrWhiteSpace(AudioDeviceId) && !AudioDeviceOptions.Any(option => option.Value == AudioDeviceId)))
            {
                _slot.AudioDeviceId = null;
            }
        }
        finally
        {
            _suppressChangedCallback = false;
        }

        OnPropertyChanged(nameof(SourceOptions));
        OnPropertyChanged(nameof(UnifiedSourceOptions));
        OnPropertyChanged(nameof(SelectedUnifiedSourceId));
        OnPropertyChanged(nameof(SelectedUnifiedSourceLabel));
        OnPropertyChanged(nameof(IsSourceMissing));
        OnPropertyChanged(nameof(ShowSourceUnavailableWarning));
        OnPropertyChanged(nameof(RowOpacity));
        OnPropertyChanged(nameof(KindLabel));
        OnPropertyChanged(nameof(IsAssigned));
        OnPropertyChanged(nameof(AudioDeviceOptions));
        OnPropertyChanged(nameof(SelectedSourceId));
        OnPropertyChanged(nameof(IsSourcePickerEnabled));
        OnPropertyChanged(nameof(AudioDeviceId));
        OnPropertyChanged(nameof(ShowAudioDevicePicker));
        OnPropertyChanged(nameof(SourceColorGradeId));
        OnPropertyChanged(nameof(SourceId));
        OnPropertyChanged(nameof(DisplayName));
        OnPropertyChanged(nameof(IsDisplayNameEditable));
        OnPropertyChanged(nameof(ShowIsoToggle));
    }

    private void SyncAudioDeviceFromCaptureDevice()
    {
        if (string.IsNullOrWhiteSpace(CaptureDeviceId))
        {
            _slot.AudioDeviceId = null;
            return;
        }

        var captureDevice = _captureDevices.FirstOrDefault(device =>
            string.Equals(device.Id, CaptureDeviceId, StringComparison.Ordinal));
        _slot.AudioDeviceId = ShowAudioDevicePicker ? captureDevice?.AssignedAudioDeviceId : null;
    }

    private void OnSlotPropertyChanged()
    {
        OnPropertyChanged(nameof(Kind));
        OnPropertyChanged(nameof(KindLabel));
        OnPropertyChanged(nameof(SelectedUnifiedSourceId));
        OnPropertyChanged(nameof(SelectedUnifiedSourceLabel));
        OnPropertyChanged(nameof(IsSourceMissing));
        OnPropertyChanged(nameof(ShowSourceUnavailableWarning));
        OnPropertyChanged(nameof(RowOpacity));
        OnPropertyChanged(nameof(IsAssigned));
        OnPropertyChanged(nameof(ParticipantId));
        OnPropertyChanged(nameof(CaptureDeviceId));
        OnPropertyChanged(nameof(AudioDeviceId));
        OnPropertyChanged(nameof(InShow));
        OnPropertyChanged(nameof(IsSourcePickerEnabled));
        OnPropertyChanged(nameof(ShowAudioDevicePicker));
        OnPropertyChanged(nameof(ShowInShowToggle));
        OnPropertyChanged(nameof(SelectedSourceId));
        OnPropertyChanged(nameof(SourceColorGradeId));
        OnPropertyChanged(nameof(SourceId));
        OnPropertyChanged(nameof(DisplayName));
        OnPropertyChanged(nameof(IsDisplayNameEditable));
        OnPropertyChanged(nameof(ShowIsoToggle));
    }

    private void OnPropertyChanged(string propertyName) =>
        PropertyChanged?.Invoke(this, new PropertyChangedEventArgs(propertyName));

    public event PropertyChangedEventHandler? PropertyChanged;
}
