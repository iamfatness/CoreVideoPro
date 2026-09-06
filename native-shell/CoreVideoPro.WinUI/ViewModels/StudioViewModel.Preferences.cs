using CommunityToolkit.Mvvm.ComponentModel;

namespace CoreVideoPro.WinUI.ViewModels;

public sealed partial class StudioViewModel
{
    // Kept for the session so subsequent successful autosaves cannot hide a
    // startup recovery/defaults warning before the operator reviews the show.
    [ObservableProperty]
    [NotifyPropertyChangedFor(nameof(HasProductionPreferencesWarning))]
    private string _productionPreferencesWarning = string.Empty;

    public bool HasProductionPreferencesWarning => !string.IsNullOrEmpty(ProductionPreferencesWarning);
}
