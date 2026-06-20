using CoreVideoPro.WinUI.Models;
using CoreVideoPro.WinUI.Services;
using System.ComponentModel;

namespace CoreVideoPro.WinUI.ViewModels;

public sealed class ShowInputSlotViewModel : INotifyPropertyChanged
{
    private readonly ShowInputSlot _slot;
    private readonly Action _onChanged;
    private readonly Action<string?, string?> _onAudioDeviceChanged;
    private IReadOnlyList<Participant> _participants = [];
    private IReadOnlyList<CaptureDevice> _captureDevices = [];
    private IReadOnlyList<AudioCaptureDevice> _audioDevices = [];
    private bool _suppressChangedCallback;

    public ShowInputSlotViewModel(
        ShowInputSlot slot,
        Action onChanged,
        Action<string?, string?>? onAudioDeviceChanged = null)
    {
        _slot = slot;
        _onChanged = onChanged;
        _onAudioDeviceChanged = onAudioDeviceChanged ?? ((_, _) => { });
        _slot.PropertyChanged += (_, _) =>
        {
            if (!_suppressChangedCallback)
            {
                _onChanged();
            }
        };
    }

    public int SlotNumber => _slot.SlotNumber;

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
            RefreshSourceOptions(_participants, _captureDevices);
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
        get => Kind == ShowInputKind.ZoomParticipant ? ParticipantId : CaptureDeviceId;
        set
        {
            if (Kind == ShowInputKind.ZoomParticipant)
            {
                ParticipantId = value;
            }
            else
            {
                CaptureDeviceId = value;
            }
        }
    }

    public string? SourceColorGradeId =>
        Kind == ShowInputKind.ZoomParticipant
            ? ParticipantId
            : string.IsNullOrWhiteSpace(CaptureDeviceId) ? null : $"capture:{CaptureDeviceId}";

    public IReadOnlyList<ShowInputKindOption> KindOptions { get; } = ShowInputRosterService.KindOptions;

    public IReadOnlyList<ShowInputSourceOption> SourceOptions { get; private set; } = [];

    public IReadOnlyList<ShowInputSourceOption> AudioDeviceOptions { get; private set; } = [];

    public void RefreshSourceOptions(
        IReadOnlyList<Participant> participants,
        IReadOnlyList<CaptureDevice> captureDevices,
        IReadOnlyList<AudioCaptureDevice>? audioDevices = null)
    {
        _participants = participants;
        _captureDevices = captureDevices;
        _audioDevices = audioDevices ?? [];
        _suppressChangedCallback = true;
        try
        {
            SourceOptions = ShowInputRosterService.BuildSourceOptions(Kind, participants, captureDevices);
            AudioDeviceOptions = ShowInputRosterService.BuildAudioSourceOptions(_audioDevices);
            if (Kind == ShowInputKind.ZoomParticipant &&
                (string.IsNullOrWhiteSpace(ParticipantId) || !SourceOptions.Any(option => option.Value == ParticipantId)))
            {
                ParticipantId = SourceOptions.FirstOrDefault()?.Value;
            }
            else if (Kind is ShowInputKind.Blackmagic or ShowInputKind.Aja or ShowInputKind.UvcWebcam or ShowInputKind.SrtIngest &&
                     (string.IsNullOrWhiteSpace(CaptureDeviceId) || !SourceOptions.Any(option => option.Value == CaptureDeviceId)))
            {
                CaptureDeviceId = SourceOptions.FirstOrDefault()?.Value;
            }

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
        OnPropertyChanged(nameof(AudioDeviceOptions));
        OnPropertyChanged(nameof(SelectedSourceId));
        OnPropertyChanged(nameof(IsSourcePickerEnabled));
        OnPropertyChanged(nameof(AudioDeviceId));
        OnPropertyChanged(nameof(ShowAudioDevicePicker));
        OnPropertyChanged(nameof(SourceColorGradeId));
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
        OnPropertyChanged(nameof(ParticipantId));
        OnPropertyChanged(nameof(CaptureDeviceId));
        OnPropertyChanged(nameof(AudioDeviceId));
        OnPropertyChanged(nameof(InShow));
        OnPropertyChanged(nameof(IsSourcePickerEnabled));
        OnPropertyChanged(nameof(ShowAudioDevicePicker));
        OnPropertyChanged(nameof(ShowInShowToggle));
        OnPropertyChanged(nameof(SelectedSourceId));
        OnPropertyChanged(nameof(SourceColorGradeId));
    }

    private void OnPropertyChanged(string propertyName) =>
        PropertyChanged?.Invoke(this, new PropertyChangedEventArgs(propertyName));

    public event PropertyChangedEventHandler? PropertyChanged;
}
