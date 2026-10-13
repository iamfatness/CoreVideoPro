using CommunityToolkit.Mvvm.ComponentModel;

namespace CoreVideoPro.WinUI.ViewModels;

public sealed partial class TilesMemberViewModel(string id) : ObservableObject
{
    public string Id { get; } = id;
    [ObservableProperty] private string _name = id;
    [ObservableProperty] private bool _excluded;
    [ObservableProperty] private bool _included;
    [ObservableProperty] private bool _canInclude;
    [ObservableProperty] private bool _isAvailable = true;
    [ObservableProperty] private string _status = "Eligible";
    public string IncludeLabel=>"Include "+Name;
    partial void OnNameChanged(string value)=>OnPropertyChanged(nameof(IncludeLabel));
}
