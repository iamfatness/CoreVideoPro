namespace CoreVideoPro.WinUI.Models;

public enum ShowInputKind
{
    Unassigned,
    ZoomParticipant,
    Blackmagic,
    Aja,
    UvcWebcam,
    Screen,
    SrtIngest,
    Media,
    Browser
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

    /// <summary>Category for the source-picker menu ("Zoom", "Camera", "Screen", "SRT",
    /// "Media") — the picker shows one submenu per group so long device lists stay
    /// filterable at pick time. Empty for entries outside any group (e.g. Missing).</summary>
    public string Group { get; init; } = string.Empty;
}

/// <summary>A multiviewer drag-drop reorder request: swap the Show Input assignments of two
/// slots (1-based slot numbers). Raised by ShowMultiviewHost when a source tile is dropped
/// onto another source tile.</summary>
public sealed record MultiviewTileSwapRequest(int FromSlot, int ToSlot);

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
    private string? _audioDeviceId;

    [CommunityToolkit.Mvvm.ComponentModel.ObservableProperty]
    private bool _inShow;

    public string SlotLabel => $"Input {SlotNumber:00}";

    public string KindLabel => Kind switch
    {
        ShowInputKind.ZoomParticipant => "Zoom",
        ShowInputKind.Blackmagic => "Blackmagic",
        ShowInputKind.Aja => "AJA",
        ShowInputKind.UvcWebcam => "UVC webcam",
        ShowInputKind.Screen => "Screen",
        ShowInputKind.SrtIngest => "SRT ingest",
        ShowInputKind.Media => "Media",
        ShowInputKind.Browser => "Browser",
        _ => "Unassigned"
    };

    public bool IsAssigned => Kind switch
    {
        ShowInputKind.ZoomParticipant or ShowInputKind.Media => !string.IsNullOrWhiteSpace(ParticipantId),
        ShowInputKind.Blackmagic or ShowInputKind.Aja or ShowInputKind.UvcWebcam or ShowInputKind.Screen or ShowInputKind.SrtIngest or ShowInputKind.Browser => !string.IsNullOrWhiteSpace(CaptureDeviceId),
        _ => false
    };

    public bool IsSourcePickerEnabled =>
        Kind is ShowInputKind.ZoomParticipant or ShowInputKind.Blackmagic or ShowInputKind.Aja or ShowInputKind.UvcWebcam or ShowInputKind.Screen or ShowInputKind.SrtIngest or ShowInputKind.Media or ShowInputKind.Browser;

    public bool IsAudioPickerEnabled =>
        Kind is ShowInputKind.UvcWebcam or ShowInputKind.Blackmagic or ShowInputKind.Aja;

    partial void OnKindChanged(ShowInputKind value)
    {
        if (value == ShowInputKind.Unassigned)
        {
            ParticipantId = null;
            CaptureDeviceId = null;
            AudioDeviceId = null;
            InShow = false;
        }
        else if (value is ShowInputKind.ZoomParticipant or ShowInputKind.Media)
        {
            CaptureDeviceId = null;
            AudioDeviceId = null;
        }
        else
        {
            ParticipantId = null;
            if (value is ShowInputKind.SrtIngest or ShowInputKind.Browser)
            {
                AudioDeviceId = null;  // no paired-audio concept (browser audio is BR-3)
            }
        }

        OnPropertyChanged(nameof(KindLabel));
        OnPropertyChanged(nameof(IsAssigned));
        OnPropertyChanged(nameof(IsSourcePickerEnabled));
        OnPropertyChanged(nameof(IsAudioPickerEnabled));
    }

    partial void OnParticipantIdChanged(string? value) => OnPropertyChanged(nameof(IsAssigned));

    partial void OnCaptureDeviceIdChanged(string? value) => OnPropertyChanged(nameof(IsAssigned));

    partial void OnAudioDeviceIdChanged(string? value) => OnPropertyChanged(nameof(IsAudioPickerEnabled));

    partial void OnInShowChanged(bool value) => OnPropertyChanged(nameof(IsAssigned));
}
