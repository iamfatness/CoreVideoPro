using CommunityToolkit.Mvvm.ComponentModel;

namespace CoreVideoPro.WinUI.ViewModels;

public sealed partial class TilesMemberViewModel(string id) : ObservableObject
{
    public string Id { get; } = id;
    [ObservableProperty] private string _name = id;
    [ObservableProperty] private bool _excluded;
    [ObservableProperty] private string _status = "Eligible";
}
