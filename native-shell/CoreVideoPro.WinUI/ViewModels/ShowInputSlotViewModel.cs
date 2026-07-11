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
    private IReadOnlyList<Participant> _participants = [];
    private IReadOnlyList<CaptureDevice> _captureDevices = [];
    private IReadOnlyList<AudioCaptureDevice> _audioDevices = [];
    private IReadOnlyList<MediaAsset> _mediaAssets = [];
    private bool _suppressChangedCallback;

    public ShowInputSlotViewModel(
        ShowInputSlot slot,
        Action onChanged,
        Action<string?, string?>? onAudioDeviceChanged = null,
        Func<string?, string, string>? resolveDisplayName = null,
        Action<string?, string?>? setDisplayName = null)
    {
        _slot = slot;
        _onChanged = onChanged;
        _onAudioDeviceChanged = onAudioDeviceChanged ?? ((_, _) => { });
        _resolveDisplayName = resolveDisplayName ?? ((_, derived) => derived);
        _setDisplayName = setDisplayName ?? ((_, _) => { });
        _slot.PropertyChanged += (_, _) =>
        {
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

            _slot.Kind = value;
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

            _slot.ParticipantId = value;
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

            _slot.CaptureDeviceId = value;
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

            _slot.InShow = value;
            OnSlotPropertyChanged();
        }
    }

    public bool IsSourcePickerEnabled => _slot.IsSourcePickerEnabled;

    public bool ShowInShowToggle => true;

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
                return;
            }

            if (value.StartsWith(ShowInputRosterService.ZoomSourcePrefix, StringComparison.Ordinal))
            {
                _slot.Kind = ShowInputKind.ZoomParticipant;
                _slot.ParticipantId = value[ShowInputRosterService.ZoomSourcePrefix.Length..];
            }
            else if (value.StartsWith(ShowInputRosterService.MediaSourcePrefix, StringComparison.Ordinal))
            {
                _slot.Kind = ShowInputKind.Media;
                // Media slots store the full "media:<assetId>" id in ParticipantId.
                _slot.ParticipantId = value;
            }
            else if (value.StartsWith(ShowInputRosterService.CaptureSourcePrefix, StringComparison.Ordinal))
            {
                var deviceId = value[ShowInputRosterService.CaptureSourcePrefix.Length..];
                var device = _captureDevices.FirstOrDefault(item =>
                    string.Equals(item.Id, deviceId, StringComparison.Ordinal));
                _slot.Kind = device is null
                    ? ShowInputKind.UvcWebcam
                    : ShowInputRosterService.InferCaptureDeviceKind(device);
                _slot.CaptureDeviceId = deviceId;
                SyncAudioDeviceFromCaptureDevice();
            }
            else
            {
                OnPropertyChanged(nameof(SelectedUnifiedSourceId));
                return;
            }

            RefreshSourceOptions(_participants, _captureDevices, _audioDevices, _mediaAssets);
            OnSlotPropertyChanged();
        }
    }

    public IReadOnlyList<ShowInputSourceOption> UnifiedSourceOptions { get; private set; } = [];

    private bool _isSourceMissing;

    /// <summary>True when the slot is assigned but its source is no longer available
    /// (device unplugged, guest left, asset removed). The binding is KEPT — the source
    /// re-attaches when it returns; we never silently substitute another source.</summary>
    public bool IsSourceMissing => _isSourceMissing;

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
        ShowInputKind.Blackmagic or ShowInputKind.Aja or ShowInputKind.UvcWebcam or ShowInputKind.Screen or ShowInputKind.SrtIngest
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
            // mid-show. A gone source now surfaces as an explicit "Missing — was …" entry and
            // the binding is kept so it re-attaches when the source returns.
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
        OnPropertyChanged(nameof(IsSourceMissing));
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
        OnPropertyChanged(nameof(IsSourceMissing));
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
    }

    private void OnPropertyChanged(string propertyName) =>
        PropertyChanged?.Invoke(this, new PropertyChangedEventArgs(propertyName));

    public event PropertyChangedEventHandler? PropertyChanged;
}
