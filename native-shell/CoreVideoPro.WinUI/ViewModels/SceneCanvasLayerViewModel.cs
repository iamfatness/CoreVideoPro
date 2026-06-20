using CommunityToolkit.Mvvm.ComponentModel;
using CoreVideoPro.WinUI.Models;
using CoreVideoPro.WinUI.Services;

namespace CoreVideoPro.WinUI.ViewModels;

public sealed partial class SceneCanvasLayerViewModel : ObservableObject
{
    private const string CaptureValuePrefix = "capture:";
    private readonly Action<SceneCanvasLayerViewModel> _onChanged;
    private readonly SourceRoute _route;
    private IReadOnlyList<Participant> _participants;
    private IReadOnlyList<CaptureDevice> _captureDevices;
    private IReadOnlyList<ShowInputSlot> _showInputs;
    private bool _suppressChangeNotification;

    public SceneCanvasLayerViewModel(
        int layerIndex,
        SourceRoute route,
        IReadOnlyList<Participant> participants,
        IReadOnlyList<CaptureDevice> captureDevices,
        IReadOnlyList<ShowInputSlot> showInputs,
        Action<SceneCanvasLayerViewModel> onChanged)
    {
        LayerIndex = layerIndex;
        _route = route;
        _onChanged = onChanged;
        _participants = participants;
        _captureDevices = captureDevices;
        _showInputs = showInputs;

        _mode = SceneRoutingService.ModeToWire(route.Mode);
        _participantId = ResolveSourceId(route, participants, captureDevices, showInputs);
        _audioRole = SceneRoutingService.AudioRoleToWire(route.AudioRole);
        _x = route.CanvasRect?.X ?? 0;
        _y = route.CanvasRect?.Y ?? 0;
        _width = route.CanvasRect?.Width ?? 1;
        _height = route.CanvasRect?.Height ?? 1;
        ParticipantOptions = BuildSourceOptions(_mode, participants, captureDevices, showInputs);
    }

    public int LayerIndex { get; }

    public string LayerLabel => $"Source {LayerIndex + 1}";

    public IReadOnlyList<RouteSelectOption> ModeOptions { get; } = SceneRoutingService.RouteModeOptions;

    public IReadOnlyList<RouteSelectOption> AudioRoleOptions { get; } = SceneRoutingService.AudioRoleOptions;

    public IReadOnlyList<RouteSelectOption> ParticipantOptions { get; private set; } = [];

    [ObservableProperty]
    private string _mode;

    [ObservableProperty]
    private string _participantId;

    [ObservableProperty]
    private string _audioRole;

    [ObservableProperty]
    private double _x;

    [ObservableProperty]
    private double _y;

    [ObservableProperty]
    private double _width;

    [ObservableProperty]
    private double _height;

    [ObservableProperty]
    private VideoSurfaceState _surface = VideoSurfaceState.Waiting(
        VideoSurfaceKind.Multiview,
        "scene-layer",
        "Source preview");

    public bool IsParticipantPickerEnabled => Mode is "fixed" or "spotlight" or "capture-input" or "active-speaker";

    partial void OnModeChanged(string value)
    {
        RefreshSourceOptions();
        OnPropertyChanged(nameof(IsParticipantPickerEnabled));
        if (_suppressChangeNotification)
        {
            return;
        }

        ApplyRoute();
        _onChanged(this);
    }

    partial void OnParticipantIdChanged(string value)
    {
        if (_suppressChangeNotification)
        {
            return;
        }

        ApplyRoute();
        _onChanged(this);
    }

    partial void OnAudioRoleChanged(string value)
    {
        if (_suppressChangeNotification)
        {
            return;
        }

        ApplyRoute();
        _onChanged(this);
    }

    public void SyncFromRoute(
        IReadOnlyList<Participant> participants,
        IReadOnlyList<CaptureDevice> captureDevices,
        IReadOnlyList<ShowInputSlot> showInputs)
    {
        _suppressChangeNotification = true;
        try
        {
            _participants = participants;
            _captureDevices = captureDevices;
            _showInputs = showInputs;
            Mode = SceneRoutingService.ModeToWire(_route.Mode);
            ParticipantOptions = BuildSourceOptions(Mode, participants, captureDevices, showInputs);
            ParticipantId = ResolveSourceId(_route, participants, captureDevices, showInputs);
            AudioRole = SceneRoutingService.AudioRoleToWire(_route.AudioRole);
            X = _route.CanvasRect?.X ?? 0;
            Y = _route.CanvasRect?.Y ?? 0;
            Width = _route.CanvasRect?.Width ?? 1;
            Height = _route.CanvasRect?.Height ?? 1;
            OnPropertyChanged(nameof(ParticipantOptions));
        }
        finally
        {
            _suppressChangeNotification = false;
        }
    }

    public void SetCanvasRect(double x, double y, double width, double height, bool notify = true)
    {
        X = x;
        Y = y;
        Width = width;
        Height = height;
        ApplyRoute();
        if (notify)
        {
            _onChanged(this);
        }
    }

    public void SetSurface(VideoSurfaceState? surface)
    {
        Surface = surface ?? VideoSurfaceState.Waiting(
            VideoSurfaceKind.Multiview,
            $"scene-layer-{LayerIndex + 1}",
            LayerLabel);
    }

    public void ApplyRoute()
    {
        _route.Mode = SceneRoutingService.ModeFromWire(Mode);
        _route.ShowInputSlotNumber = TryParseShowInputValue(ParticipantId, out var slotNumber)
            ? slotNumber
            : null;

        if (_route.ShowInputSlotNumber is { } showInputSlotNumber &&
            _showInputs.FirstOrDefault(slot => slot.SlotNumber == showInputSlotNumber) is { } showInput)
        {
            _route.ParticipantId = showInput.Kind == ShowInputKind.ZoomParticipant ? showInput.ParticipantId : null;
            _route.CaptureDeviceId = showInput.Kind is ShowInputKind.Blackmagic or ShowInputKind.Aja or ShowInputKind.UvcWebcam
                ? showInput.CaptureDeviceId
                : null;
            _route.Mode = showInput.Kind == ShowInputKind.ZoomParticipant
                ? SourceRouteMode.Fixed
                : SourceRouteMode.CaptureDevice;
        }
        else
        {
            if (TryParseCaptureDeviceValue(ParticipantId, out var captureDeviceId))
            {
                _route.Mode = SourceRouteMode.CaptureDevice;
                _route.ParticipantId = null;
                _route.CaptureDeviceId = captureDeviceId;
            }
            else if (Mode is "fixed" or "spotlight")
            {
                _route.ParticipantId = ParticipantId;
                _route.CaptureDeviceId = null;
            }
            else if (Mode is "active-speaker" &&
                _participants.Any(participant => string.Equals(participant.Id, ParticipantId, StringComparison.Ordinal)))
            {
                _route.Mode = SourceRouteMode.Fixed;
                _route.ParticipantId = ParticipantId;
                _route.CaptureDeviceId = null;
            }
            else if (Mode is "capture-input")
            {
                _route.ParticipantId = null;
                _route.CaptureDeviceId = ParticipantId;
            }
            else
            {
                _route.ParticipantId = null;
                _route.CaptureDeviceId = null;
            }
        }

        SetModeSilently(SceneRoutingService.ModeToWire(_route.Mode));

        _route.AudioRole = SceneRoutingService.AudioRoleFromWire(AudioRole);
        _route.CanvasRect = new NormalizedCanvasRect
        {
            X = X,
            Y = Y,
            Width = Width,
            Height = Height
        };
        _route.CanvasRect.Clamp();
        _route.ZIndex = LayerIndex;
    }

    private void RefreshSourceOptions()
    {
        ParticipantOptions = BuildSourceOptions(Mode, _participants, _captureDevices, _showInputs);
        if (!ParticipantOptions.Any(option => option.Value == ParticipantId))
        {
            ParticipantId = ParticipantOptions.FirstOrDefault()?.Value ?? string.Empty;
        }

        OnPropertyChanged(nameof(ParticipantOptions));
    }

    private void SetModeSilently(string mode)
    {
        if (string.Equals(Mode, mode, StringComparison.Ordinal))
        {
            return;
        }

        _suppressChangeNotification = true;
        try
        {
            Mode = mode;
        }
        finally
        {
            _suppressChangeNotification = false;
        }
    }

    private static string ResolveSourceId(
        SourceRoute route,
        IReadOnlyList<Participant> participants,
        IReadOnlyList<CaptureDevice> captureDevices,
        IReadOnlyList<ShowInputSlot> showInputs)
    {
        if (route.ShowInputSlotNumber is { } slotNumber &&
            showInputs.Any(slot => slot.SlotNumber == slotNumber))
        {
            return FormatShowInputValue(slotNumber);
        }

        return route.Mode == SourceRouteMode.CaptureDevice
            ? FormatCaptureDeviceValue(route.CaptureDeviceId ?? captureDevices.FirstOrDefault()?.Id ?? string.Empty)
            : route.ParticipantId ?? participants.FirstOrDefault()?.Id ?? string.Empty;
    }

    private static IReadOnlyList<RouteSelectOption> BuildSourceOptions(
        string mode,
        IReadOnlyList<Participant> participants,
        IReadOnlyList<CaptureDevice> captureDevices,
        IReadOnlyList<ShowInputSlot> showInputs)
    {
        var showInputOptions = showInputs
            .Where(slot => slot.InShow && slot.IsAssigned)
            .Select(slot => new RouteSelectOption
            {
                Value = FormatShowInputValue(slot.SlotNumber),
                Label = $"{slot.SlotLabel} - {ResolveShowInputSourceLabel(slot, participants, captureDevices)}"
            })
            .ToList();

        var captureOptions = captureDevices
            .Select(device => new RouteSelectOption
            {
                Value = FormatCaptureDeviceValue(device.Id),
                Label = $"{device.Name} - {device.FormatLabel}"
            });

        var participantOptions = participants
            .Select(participant => new RouteSelectOption
            {
                Value = participant.Id,
                Label = participant.Name
            });

        return showInputOptions
            .Concat(captureOptions)
            .Concat(participantOptions)
            .ToList();
    }

    private static string FormatShowInputValue(int slotNumber) => $"input-{slotNumber:00}";

    private static string FormatCaptureDeviceValue(string value) =>
        string.IsNullOrWhiteSpace(value) || value.StartsWith(CaptureValuePrefix, StringComparison.OrdinalIgnoreCase)
            ? value
            : $"{CaptureValuePrefix}{value}";

    private static bool TryParseShowInputValue(string? value, out int slotNumber)
    {
        slotNumber = 0;
        return value is { Length: 8 } &&
            value.StartsWith("input-", StringComparison.OrdinalIgnoreCase) &&
            int.TryParse(value[6..], out slotNumber);
    }

    private static bool TryParseCaptureDeviceValue(string? value, out string captureDeviceId)
    {
        captureDeviceId = string.Empty;
        if (string.IsNullOrWhiteSpace(value) ||
            !value.StartsWith(CaptureValuePrefix, StringComparison.OrdinalIgnoreCase))
        {
            return false;
        }

        captureDeviceId = value[CaptureValuePrefix.Length..];
        return captureDeviceId.Length > 0;
    }

    private static string ResolveShowInputSourceLabel(
        ShowInputSlot slot,
        IReadOnlyList<Participant> participants,
        IReadOnlyList<CaptureDevice> captureDevices)
    {
        if (slot.Kind == ShowInputKind.ZoomParticipant &&
            slot.ParticipantId is { Length: > 0 } participantId)
        {
            return participants.FirstOrDefault(participant => participant.Id == participantId)?.Name ??
                participantId;
        }

        if (slot.CaptureDeviceId is { Length: > 0 } captureDeviceId)
        {
            return captureDevices.FirstOrDefault(device => device.Id == captureDeviceId)?.Name ??
                captureDeviceId;
        }

        return slot.KindLabel;
    }
}
