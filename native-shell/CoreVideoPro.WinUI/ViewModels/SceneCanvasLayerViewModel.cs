using CommunityToolkit.Mvvm.ComponentModel;
using CoreVideoPro.WinUI.Models;
using CoreVideoPro.WinUI.Services;

namespace CoreVideoPro.WinUI.ViewModels;

public sealed partial class SceneCanvasLayerViewModel : ObservableObject
{
    private readonly Action<SceneCanvasLayerViewModel> _onChanged;
    private readonly SourceRoute _route;
    private bool _suppressChangeNotification;

    public SceneCanvasLayerViewModel(
        int layerIndex,
        SourceRoute route,
        IReadOnlyList<Participant> participants,
        Action<SceneCanvasLayerViewModel> onChanged)
    {
        LayerIndex = layerIndex;
        _route = route;
        _onChanged = onChanged;
        ParticipantOptions = participants
            .Select(participant => new RouteSelectOption
            {
                Value = participant.Id,
                Label = participant.Name
            })
            .ToList();

        _mode = SceneRoutingService.ModeToWire(route.Mode);
        _participantId = route.ParticipantId ?? participants.FirstOrDefault()?.Id ?? string.Empty;
        _audioRole = SceneRoutingService.AudioRoleToWire(route.AudioRole);
        _x = route.CanvasRect?.X ?? 0;
        _y = route.CanvasRect?.Y ?? 0;
        _width = route.CanvasRect?.Width ?? 1;
        _height = route.CanvasRect?.Height ?? 1;
    }

    public int LayerIndex { get; }

    public string LayerLabel => $"Source {LayerIndex + 1}";

    public IReadOnlyList<RouteSelectOption> ModeOptions { get; } = SceneRoutingService.RouteModeOptions;

    public IReadOnlyList<RouteSelectOption> AudioRoleOptions { get; } = SceneRoutingService.AudioRoleOptions;

    public IReadOnlyList<RouteSelectOption> ParticipantOptions { get; }

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

    public bool IsParticipantPickerEnabled => Mode is "fixed" or "spotlight";

    partial void OnModeChanged(string value)
    {
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

    public void SyncFromRoute(IReadOnlyList<Participant> participants)
    {
        _suppressChangeNotification = true;
        try
        {
            Mode = SceneRoutingService.ModeToWire(_route.Mode);
            ParticipantId = _route.ParticipantId ?? participants.FirstOrDefault()?.Id ?? string.Empty;
            AudioRole = SceneRoutingService.AudioRoleToWire(_route.AudioRole);
            X = _route.CanvasRect?.X ?? 0;
            Y = _route.CanvasRect?.Y ?? 0;
            Width = _route.CanvasRect?.Width ?? 1;
            Height = _route.CanvasRect?.Height ?? 1;
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

    public void ApplyRoute()
    {
        _route.Mode = SceneRoutingService.ModeFromWire(Mode);
        _route.ParticipantId = IsParticipantPickerEnabled ? ParticipantId : null;
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
}