using CoreVideoPro.WinUI.Models;
using CoreVideoPro.WinUI.Services;
using System.ComponentModel;

namespace CoreVideoPro.WinUI.ViewModels;

public sealed class ShowInputSlotViewModel : INotifyPropertyChanged
{
    private readonly ShowInputSlot _slot;
    private readonly Action _onChanged;
    private bool _suppressChangedCallback;

    public ShowInputSlotViewModel(ShowInputSlot slot, Action onChanged)
    {
        _slot = slot;
        _onChanged = onChanged;
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

    public IReadOnlyList<ShowInputKindOption> KindOptions { get; } = ShowInputRosterService.KindOptions;

    public IReadOnlyList<ShowInputSourceOption> SourceOptions { get; private set; } = [];

    public void RefreshSourceOptions(
        IReadOnlyList<Participant> participants,
        IReadOnlyList<CaptureDevice> captureDevices)
    {
        _suppressChangedCallback = true;
        try
        {
            SourceOptions = ShowInputRosterService.BuildSourceOptions(Kind, participants, captureDevices);
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
        }
        finally
        {
            _suppressChangedCallback = false;
        }

        OnPropertyChanged(nameof(SourceOptions));
        OnPropertyChanged(nameof(SelectedSourceId));
        OnPropertyChanged(nameof(IsSourcePickerEnabled));
    }

    private void OnSlotPropertyChanged()
    {
        OnPropertyChanged(nameof(Kind));
        OnPropertyChanged(nameof(ParticipantId));
        OnPropertyChanged(nameof(CaptureDeviceId));
        OnPropertyChanged(nameof(InShow));
        OnPropertyChanged(nameof(IsSourcePickerEnabled));
        OnPropertyChanged(nameof(ShowInShowToggle));
        OnPropertyChanged(nameof(SelectedSourceId));
    }

    private void OnPropertyChanged(string propertyName) =>
        PropertyChanged?.Invoke(this, new PropertyChangedEventArgs(propertyName));

    public event PropertyChangedEventHandler? PropertyChanged;
}
