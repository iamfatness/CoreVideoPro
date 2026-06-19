namespace CoreVideoPro.WinUI.Models;

public enum ShowInputKind
{
    Unassigned,
    ZoomParticipant,
    Blackmagic,
    Aja,
    UvcWebcam
}

public sealed class ShowInputKindOption
{
    public required ShowInputKind Value { get; init; }
    public required string Label { get; init; }
}

public sealed class ShowInputSourceOption
{
    public required string Value { get; init; }
    public required string Label { get; init; }
}

public sealed partial class ShowInputSlot : CommunityToolkit.Mvvm.ComponentModel.ObservableObject
{
    public int SlotNumber { get; init; }

    [CommunityToolkit.Mvvm.ComponentModel.ObservableProperty]
    private ShowInputKind _kind = ShowInputKind.Unassigned;

    [CommunityToolkit.Mvvm.ComponentModel.ObservableProperty]
    private string? _participantId;

    [CommunityToolkit.Mvvm.ComponentModel.ObservableProperty]
    private string? _captureDeviceId;

    [CommunityToolkit.Mvvm.ComponentModel.ObservableProperty]
    private bool _inShow;

    public string SlotLabel => $"Input {SlotNumber:00}";

    public string KindLabel => Kind switch
    {
        ShowInputKind.ZoomParticipant => "Zoom",
        ShowInputKind.Blackmagic => "Blackmagic",
        ShowInputKind.Aja => "AJA",
        ShowInputKind.UvcWebcam => "UVC webcam",
        _ => "Unassigned"
    };

    public bool IsAssigned => Kind switch
    {
        ShowInputKind.ZoomParticipant => !string.IsNullOrWhiteSpace(ParticipantId),
        ShowInputKind.Blackmagic or ShowInputKind.Aja or ShowInputKind.UvcWebcam => !string.IsNullOrWhiteSpace(CaptureDeviceId),
        _ => false
    };

    public bool IsSourcePickerEnabled =>
        Kind is ShowInputKind.ZoomParticipant or ShowInputKind.Blackmagic or ShowInputKind.Aja or ShowInputKind.UvcWebcam;

    partial void OnKindChanged(ShowInputKind value)
    {
        if (value == ShowInputKind.Unassigned)
        {
            ParticipantId = null;
            CaptureDeviceId = null;
            InShow = false;
        }
        else if (value == ShowInputKind.ZoomParticipant)
        {
            CaptureDeviceId = null;
        }
        else
        {
            ParticipantId = null;
        }

        OnPropertyChanged(nameof(KindLabel));
        OnPropertyChanged(nameof(IsAssigned));
        OnPropertyChanged(nameof(IsSourcePickerEnabled));
    }

    partial void OnParticipantIdChanged(string? value) => OnPropertyChanged(nameof(IsAssigned));

    partial void OnCaptureDeviceIdChanged(string? value) => OnPropertyChanged(nameof(IsAssigned));

    partial void OnInShowChanged(bool value) => OnPropertyChanged(nameof(IsAssigned));
}
