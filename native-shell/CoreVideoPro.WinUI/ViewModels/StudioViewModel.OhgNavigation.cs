using CommunityToolkit.Mvvm.ComponentModel;
using CoreVideoPro.WinUI.Models;
using Microsoft.UI.Xaml;

namespace CoreVideoPro.WinUI.ViewModels;

public sealed partial class StudioViewModel
{
    [ObservableProperty]
    [NotifyPropertyChangedFor(nameof(OhgShowNavigationVisibility))]
    private bool _ohgShowEnabled;

    public Visibility OhgShowNavigationVisibility => OhgShowEnabled ? Visibility.Visible : Visibility.Collapsed;

    partial void OnOhgShowEnabledChanged(bool value)
    {
        ActiveTab = StudioTabPlumbing.AvailableTab(ActiveTab, value);
        SaveProductionOutputPreferences();
    }

    private bool KeepNavigationAvailable(StudioTab requested)
    {
        var available = StudioTabPlumbing.AvailableTab(requested, OhgShowEnabled);
        if (available == requested) return true;
        ActiveTab = available;
        return false;
    }
}
