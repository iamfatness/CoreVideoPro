using CommunityToolkit.Mvvm.ComponentModel;
using CoreVideoPro.WinUI.Services;

namespace CoreVideoPro.WinUI.ViewModels;

/// <summary>One panelist row (the roster list and the unseated list share this shape), mirroring
/// <see cref="OhgPanelistRow"/> field-for-field plus the page's <see cref="IsSelected"/>.
///
/// Rows are UPDATED IN PLACE by <see cref="ObservableCollectionSync"/> — a row instance outlives
/// every snapshot in which its <see cref="Key"/> is still present — so <see cref="IsSelected"/>
/// (page state, not engine state) survives ingestion for free.</summary>
public sealed partial class OhgPanelistRowViewModel : ObservableObject
{
    public OhgPanelistRowViewModel(OhgPanelistRow row)
    {
        Key = row.ParticipantId;
        Update(row);
    }

    /// <summary>The sync key: the participant id. Immutable for the row's lifetime.</summary>
    public string Key { get; }

    [ObservableProperty] private string participantId = "";
    [ObservableProperty] private string displayName = "";
    [ObservableProperty] private string location = "";
    [ObservableProperty] private string? pin;
    [ObservableProperty] private bool hasMukana;
    [ObservableProperty] private string role = "panelist";
    [ObservableProperty] private bool online;
    [ObservableProperty] private bool videoOn;
    [ObservableProperty] private bool audioOn;
    [ObservableProperty] private bool handRaised;
    [ObservableProperty] private int? slot;
    [ObservableProperty] private bool isSelected;

    /// <summary>Every setter is a generated <c>SetProperty</c>, which raises ONLY on a real change
    /// — so re-applying an unchanged snapshot row costs no PropertyChanged traffic.</summary>
    public void Update(OhgPanelistRow row)
    {
        ParticipantId = row.ParticipantId;
        DisplayName = row.DisplayName;
        Location = row.Location;
        Pin = row.Pin;
        HasMukana = row.HasMukana;
        Role = row.Role;
        Online = row.Online;
        VideoOn = row.VideoOn;
        AudioOn = row.AudioOn;
        HandRaised = row.HandRaised;
        Slot = row.Slot;
    }
}
