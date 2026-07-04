using CommunityToolkit.Mvvm.ComponentModel;
using CommunityToolkit.Mvvm.Input;
using CoreVideoPro.WinUI.Models;
using CoreVideoPro.WinUI.Services;

namespace CoreVideoPro.WinUI.ViewModels;

public sealed partial class SceneCanvasLayerViewModel : ObservableObject
{
    private const string CaptureValuePrefix = "capture:";
    private const string MediaValuePrefix = "media:";
    private readonly Action<SceneCanvasLayerViewModel> _onChanged;
    private readonly SourceRoute _route;
    private IReadOnlyList<Participant> _participants;
    private IReadOnlyList<CaptureDevice> _captureDevices;
    private IReadOnlyList<ShowInputSlot> _showInputs;
    private IReadOnlyList<MediaAsset> _mediaAssets;
    private bool _suppressChangeNotification;

    public SceneCanvasLayerViewModel(
        int layerIndex,
        SourceRoute route,
        IReadOnlyList<Participant> participants,
        IReadOnlyList<CaptureDevice> captureDevices,
        IReadOnlyList<ShowInputSlot> showInputs,
        IReadOnlyList<MediaAsset> mediaAssets,
        Action<SceneCanvasLayerViewModel> onChanged)
    {
        LayerIndex = layerIndex;
        _route = route;
        _onChanged = onChanged;
        _participants = participants;
        _captureDevices = captureDevices;
        _showInputs = showInputs;
        _mediaAssets = mediaAssets;

        _mode = SceneRoutingService.ModeToWire(route.Mode);
        _participantId = ResolveSourceId(route, participants, captureDevices, showInputs, mediaAssets);
        _audioRole = SceneRoutingService.AudioRoleToWire(route.AudioRole);
        _x = route.CanvasRect?.X ?? 0;
        _y = route.CanvasRect?.Y ?? 0;
        _width = route.CanvasRect?.Width ?? 1;
        _height = route.CanvasRect?.Height ?? 1;
        _fitMode = SceneRoutingService.NormalizeFitMode(route.FitMode);
        _borderStyle = SceneRoutingService.NormalizeBorderStyle(route.BorderStyle);
        _borderColor = SceneRoutingService.NormalizeBorderColor(route.BorderColor);
        _borderThickness = Math.Clamp(route.BorderThickness, 0, 12);
        _sourceScale = SceneRoutingService.NormalizeSourceScale(route.SourceScale);
        _sourceOffsetX = SceneRoutingService.NormalizeSourceOffset(route.SourceOffsetX);
        _sourceOffsetY = SceneRoutingService.NormalizeSourceOffset(route.SourceOffsetY);
        _opacityPercent = Math.Clamp(route.Opacity, 0.1, 1.0) * 100;
        ParticipantOptions = BuildSourceOptions(_mode, participants, captureDevices, showInputs, mediaAssets);
    }

    public int LayerIndex { get; }

    public string LayerLabel => $"Source {LayerIndex + 1}";

    public IReadOnlyList<RouteSelectOption> ModeOptions { get; } = SceneRoutingService.RouteModeOptions;

    public IReadOnlyList<RouteSelectOption> AudioRoleOptions { get; } = SceneRoutingService.AudioRoleOptions;

    public IReadOnlyList<RouteSelectOption> FitModeOptions { get; } =
    [
        new() { Value = "fill", Label = "Fill / crop" },
        new() { Value = "fit", Label = "Fit / letterbox" },
        new() { Value = "stretch", Label = "Stretch" }
    ];

    public string FramingSummary =>
        $"Center cut | zoom {SourceScale:0.##} | pan {SourceOffsetX:0.##} | tilt {SourceOffsetY:0.##}";

    public IReadOnlyList<RouteSelectOption> BorderStyleOptions { get; } =
    [
        new() { Value = "accent", Label = "Accent" },
        new() { Value = "solid", Label = "Custom color" },
        new() { Value = "program", Label = "Program amber" },
        new() { Value = "warning", Label = "Warning red" },
        new() { Value = "none", Label = "None" }
    ];

    public IReadOnlyList<RouteSelectOption> ParticipantOptions { get; private set; } = [];

    public string SourceColorGradeId => ResolveColorGradeSourceId(_route) ?? ParticipantId;

    public string ColorGradeSummary => _route.ColorGrade?.Summary ?? "Neutral";

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
    private string _fitMode;

    [ObservableProperty]
    private string _borderStyle;

    [ObservableProperty]
    private string _borderColor;

    [ObservableProperty]
    private double _borderThickness;

    [ObservableProperty]
    private double _sourceScale;

    [ObservableProperty]
    private double _sourceOffsetX;

    [ObservableProperty]
    private double _sourceOffsetY;

    // Per-layer opacity as a percentage (10..100). Floored at 10% in the UI so
    // a layer can't be made invisible by accident; the wire carries 0..1.
    [ObservableProperty]
    private double _opacityPercent = 100;

    public string OpacityLabel => $"{OpacityPercent:0}%";

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

    partial void OnFitModeChanged(string value) => ApplyVisualChange();

    partial void OnBorderStyleChanged(string value) => ApplyVisualChange();

    partial void OnBorderColorChanged(string value) => ApplyVisualChange();

    partial void OnBorderThicknessChanged(double value) => ApplyVisualChange();

    partial void OnSourceScaleChanged(double value) => ApplyVisualChange();

    partial void OnSourceOffsetXChanged(double value) => ApplyVisualChange();

    partial void OnSourceOffsetYChanged(double value) => ApplyVisualChange();

    partial void OnOpacityPercentChanged(double value)
    {
        OnPropertyChanged(nameof(OpacityLabel));
        ApplyVisualChange();
    }

    [RelayCommand]
    private void ResetFraming()
    {
        SourceScale = SourceRouteVisualDefaults.SourceScale;
        SourceOffsetX = SourceRouteVisualDefaults.SourceOffsetX;
        SourceOffsetY = SourceRouteVisualDefaults.SourceOffsetY;
        FitMode = SourceRouteVisualDefaults.FitMode;
    }

    public void SyncFromRoute(
        IReadOnlyList<Participant> participants,
        IReadOnlyList<CaptureDevice> captureDevices,
        IReadOnlyList<ShowInputSlot> showInputs,
        IReadOnlyList<MediaAsset> mediaAssets)
    {
        _suppressChangeNotification = true;
        try
        {
            _participants = participants;
            _captureDevices = captureDevices;
            _showInputs = showInputs;
            _mediaAssets = mediaAssets;
            Mode = SceneRoutingService.ModeToWire(_route.Mode);
            ParticipantOptions = BuildSourceOptions(Mode, participants, captureDevices, showInputs, mediaAssets);
            ParticipantId = ResolveSourceId(_route, participants, captureDevices, showInputs, mediaAssets);
            AudioRole = SceneRoutingService.AudioRoleToWire(_route.AudioRole);
            X = _route.CanvasRect?.X ?? 0;
            Y = _route.CanvasRect?.Y ?? 0;
            Width = _route.CanvasRect?.Width ?? 1;
            Height = _route.CanvasRect?.Height ?? 1;
            FitMode = SceneRoutingService.NormalizeFitMode(_route.FitMode);
            BorderStyle = SceneRoutingService.NormalizeBorderStyle(_route.BorderStyle);
            BorderColor = SceneRoutingService.NormalizeBorderColor(_route.BorderColor);
            BorderThickness = Math.Clamp(_route.BorderThickness, 0, 12);
            SourceScale = SceneRoutingService.NormalizeSourceScale(_route.SourceScale);
            SourceOffsetX = SceneRoutingService.NormalizeSourceOffset(_route.SourceOffsetX);
            SourceOffsetY = SceneRoutingService.NormalizeSourceOffset(_route.SourceOffsetY);
            OpacityPercent = Math.Clamp(_route.Opacity, 0.1, 1.0) * 100;
            OnPropertyChanged(nameof(SourceColorGradeId));
            OnPropertyChanged(nameof(ColorGradeSummary));
            OnPropertyChanged(nameof(FramingSummary));
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

    public void SetSourceOffset(double sourceOffsetX, double sourceOffsetY, bool notify = true)
    {
        SourceOffsetX = SceneRoutingService.NormalizeSourceOffset(sourceOffsetX);
        SourceOffsetY = SceneRoutingService.NormalizeSourceOffset(sourceOffsetY);
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
            _route.CaptureDeviceId = showInput.Kind is ShowInputKind.Blackmagic or ShowInputKind.Aja or ShowInputKind.UvcWebcam or ShowInputKind.SrtIngest
                ? showInput.CaptureDeviceId
                : null;
            _route.Mode = showInput.Kind == ShowInputKind.ZoomParticipant
                ? SourceRouteMode.Fixed
                : SourceRouteMode.CaptureDevice;
        }
        else
        {
            if (TryParseMediaAssetValue(ParticipantId, out var mediaAssetId))
            {
                _route.Mode = SourceRouteMode.Fixed;
                _route.ParticipantId = ShowInputRosterService.ToMediaSourceId(mediaAssetId);
                _route.CaptureDeviceId = null;
                _route.ShowInputSlotNumber = null;
                _route.SpotlightIndex = null;
            }
            else if (TryParseCaptureDeviceValue(ParticipantId, out var captureDeviceId))
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
        _route.FitMode = SceneRoutingService.NormalizeFitMode(FitMode);
        _route.BorderStyle = SceneRoutingService.NormalizeBorderStyle(BorderStyle);
        _route.BorderColor = SceneRoutingService.NormalizeBorderColor(BorderColor);
        _route.BorderThickness = Math.Clamp(BorderThickness, 0, 12);
        _route.SourceScale = SceneRoutingService.NormalizeSourceScale(SourceScale);
        _route.SourceOffsetX = SceneRoutingService.NormalizeSourceOffset(SourceOffsetX);
        _route.SourceOffsetY = SceneRoutingService.NormalizeSourceOffset(SourceOffsetY);
        _route.Opacity = Math.Clamp(OpacityPercent / 100.0, 0.1, 1.0);
        _route.SourceFramingModified = SceneRoutingService.HasModifiedSourceFraming(
            _route.FitMode,
            _route.SourceScale,
            _route.SourceOffsetX,
            _route.SourceOffsetY);
        _route.ZIndex = LayerIndex;
        OnPropertyChanged(nameof(SourceColorGradeId));
        OnPropertyChanged(nameof(ColorGradeSummary));
        OnPropertyChanged(nameof(FramingSummary));
    }

    private void ApplyVisualChange()
    {
        if (_suppressChangeNotification)
        {
            return;
        }

        ApplyRoute();
        _onChanged(this);
    }

    private void RefreshSourceOptions()
    {
        ParticipantOptions = BuildSourceOptions(Mode, _participants, _captureDevices, _showInputs, _mediaAssets);
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
        IReadOnlyList<ShowInputSlot> showInputs,
        IReadOnlyList<MediaAsset> mediaAssets)
    {
        if (route.ShowInputSlotNumber is { } slotNumber &&
            showInputs.Any(slot => slot.SlotNumber == slotNumber))
        {
            return FormatShowInputValue(slotNumber);
        }

        if (ShowInputRosterService.TryGetMediaAssetId(route.ParticipantId, out var mediaAssetId) &&
            mediaAssets.Any(asset => string.Equals(asset.Id, mediaAssetId, StringComparison.Ordinal)))
        {
            return FormatMediaAssetValue(mediaAssetId);
        }

        return route.Mode == SourceRouteMode.CaptureDevice
            ? FormatCaptureDeviceValue(route.CaptureDeviceId ?? captureDevices.FirstOrDefault()?.Id ?? string.Empty)
            : route.ParticipantId ?? participants.FirstOrDefault()?.Id ?? string.Empty;
    }

    private static IReadOnlyList<RouteSelectOption> BuildSourceOptions(
        string mode,
        IReadOnlyList<Participant> participants,
        IReadOnlyList<CaptureDevice> captureDevices,
        IReadOnlyList<ShowInputSlot> showInputs,
        IReadOnlyList<MediaAsset> mediaAssets)
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

        var mediaOptions = mediaAssets
            .Where(IsVisualMediaAsset)
            .Select(asset => new RouteSelectOption
            {
                Value = FormatMediaAssetValue(asset.Id),
                Label = $"Media - {asset.Name}"
            });

        return showInputOptions
            .Concat(captureOptions)
            .Concat(mediaOptions)
            .Concat(participantOptions)
            .ToList();
    }

    private static string FormatShowInputValue(int slotNumber) => $"input-{slotNumber:00}";

    private static string FormatMediaAssetValue(string value) =>
        string.IsNullOrWhiteSpace(value) || value.StartsWith(MediaValuePrefix, StringComparison.OrdinalIgnoreCase)
            ? value
            : $"{MediaValuePrefix}{value}";

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

    private static bool TryParseMediaAssetValue(string? value, out string mediaAssetId)
    {
        mediaAssetId = string.Empty;
        if (string.IsNullOrWhiteSpace(value) ||
            !value.StartsWith(MediaValuePrefix, StringComparison.OrdinalIgnoreCase))
        {
            return false;
        }

        mediaAssetId = value[MediaValuePrefix.Length..];
        return mediaAssetId.Length > 0;
    }

    private static bool IsVisualMediaAsset(MediaAsset asset) =>
        asset.Kind.Equals("video", StringComparison.OrdinalIgnoreCase) ||
        asset.Kind.Equals("image", StringComparison.OrdinalIgnoreCase) ||
        asset.Kind.Equals("lower-third", StringComparison.OrdinalIgnoreCase);

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

    private static string? ResolveColorGradeSourceId(SourceRoute route)
    {
        if (route.Mode == SourceRouteMode.CaptureDevice && route.CaptureDeviceId is { Length: > 0 } captureDeviceId)
        {
            return FormatCaptureDeviceValue(captureDeviceId);
        }

        return route.ParticipantId;
    }
}
