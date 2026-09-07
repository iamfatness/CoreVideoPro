using CommunityToolkit.Mvvm.ComponentModel;
using CoreVideoPro.WinUI.Services;

namespace CoreVideoPro.WinUI.ViewModels;

/// <summary>One seat in the show (fixed length = the engine's capacity), mirroring
/// <see cref="OhgSlotRow"/>. The seated panelist is flattened onto the row so the seat grid can
/// bind scalars directly; <see cref="Panelist"/> keeps the whole record for callers that need it.
/// An EMPTY seat keeps its row instance and simply reports <see cref="IsOccupied"/> false — the
/// row is never removed and re-added, so a capacity-length collection never churns structurally.</summary>
public sealed partial class OhgSlotRowViewModel : ObservableObject
{
    public OhgSlotRowViewModel(OhgSlotRow row)
    {
        Key = row.Slot;
        Update(row);
    }

    /// <summary>The sync key: the slot number. Immutable for the row's lifetime.</summary>
    public int Key { get; }

    [ObservableProperty] private int slot;
    [ObservableProperty] private bool onAir;
    [ObservableProperty] private OhgPanelistRow? panelist;
    [ObservableProperty] private bool isOccupied;
    [ObservableProperty] private string displayName = "";
    [ObservableProperty] private string location = "";
    [ObservableProperty] private string? participantId;
    [ObservableProperty] private string role = "";
    [ObservableProperty] private bool online;
    [ObservableProperty] private bool videoOn;
    [ObservableProperty] private bool audioOn;
    [ObservableProperty] private bool handRaised;
    [ObservableProperty] private bool isSelected;

    public void Update(OhgSlotRow row)
    {
        Slot = row.Slot;
        OnAir = row.OnAir;
        Panelist = row.Panelist;
        IsOccupied = row.Panelist != null;
        DisplayName = row.Panelist?.DisplayName ?? "";
        Location = row.Panelist?.Location ?? "";
        ParticipantId = row.Panelist?.ParticipantId;
        Role = row.Panelist?.Role ?? "";
        Online = row.Panelist?.Online ?? false;
        VideoOn = row.Panelist?.VideoOn ?? false;
        AudioOn = row.Panelist?.AudioOn ?? false;
        HandRaised = row.Panelist?.HandRaised ?? false;
    }
}
