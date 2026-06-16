using CommunityToolkit.Mvvm.ComponentModel;
using CoreVideoPro.WinUI.Models;
using CoreVideoPro.WinUI.Services;

namespace CoreVideoPro.WinUI.ViewModels;

public sealed partial class SlotEditorItemViewModel : ObservableObject
{
    private readonly Action<SlotEditorItemViewModel> _onChanged;

    public SlotEditorItemViewModel(
        int slotIndex,
        SourceRoute route,
        IReadOnlyList<Participant> participants,
        Action<SlotEditorItemViewModel> onChanged)
    {
        SlotIndex = slotIndex;
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
    }

    public int SlotIndex { get; }

    public string SlotLabel => $"Slot {SlotIndex + 1}";

    public IReadOnlyList<RouteSelectOption> ModeOptions { get; } = SceneRoutingService.RouteModeOptions;

    public IReadOnlyList<RouteSelectOption> AudioRoleOptions { get; } = SceneRoutingService.AudioRoleOptions;

    public IReadOnlyList<RouteSelectOption> ParticipantOptions { get; }

    [ObservableProperty]
    private string _mode;

    [ObservableProperty]
    private string _participantId;

    [ObservableProperty]
    private string _audioRole;

    public bool IsParticipantPickerEnabled =>
        Mode is "fixed" or "spotlight";

    partial void OnModeChanged(string value)
    {
        OnPropertyChanged(nameof(IsParticipantPickerEnabled));
        _onChanged(this);
    }

    partial void OnParticipantIdChanged(string value) => _onChanged(this);

    partial void OnAudioRoleChanged(string value) => _onChanged(this);

    public void ApplyTo(SourceRoute route)
    {
        route.Mode = SceneRoutingService.ModeFromWire(Mode);
        route.ParticipantId = IsParticipantPickerEnabled ? ParticipantId : null;
        route.AudioRole = SceneRoutingService.AudioRoleFromWire(AudioRole);
    }
}