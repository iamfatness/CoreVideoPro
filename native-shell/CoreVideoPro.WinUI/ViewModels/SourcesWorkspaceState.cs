using CommunityToolkit.Mvvm.ComponentModel;
using CoreVideoPro.WinUI.Models;
using CoreVideoPro.WinUI.Services;

namespace CoreVideoPro.WinUI.ViewModels;

/// <summary>UI-only selection/filter/layout state. Never writes source or production models.</summary>
public sealed partial class SourcesWorkspaceState : ObservableObject
{
    public static IReadOnlyList<string> Categories { get; } =
        ["All sources", "Zoom", "Cameras", "Screens", "Media", "Network", "Browser", "Unassigned"];
    [ObservableProperty] private ShowInputSlotViewModel? _selectedInput;
    [ObservableProperty] private bool _inspectorOpen;
    [ObservableProperty] private string _query = "";
    [ObservableProperty] private string _category = "All sources";
    [ObservableProperty] private SourcesWorkspacePreferences _columns = new();

    public void Inspect(ShowInputSlotViewModel input) { SelectedInput = input; InspectorOpen = true; }
    public bool Matches(ShowInputSlotViewModel input)
    {
        var category = input.Kind switch
        {
            ShowInputKind.ZoomParticipant => "Zoom",
            ShowInputKind.UvcWebcam or ShowInputKind.Blackmagic or ShowInputKind.Aja => "Cameras",
            ShowInputKind.Screen => "Screens",
            ShowInputKind.Media => "Media",
            ShowInputKind.SrtIngest or ShowInputKind.RtmpIngest or ShowInputKind.Ndi => "Network",
            ShowInputKind.Browser => "Browser",
            _ => "Unassigned"
        };
        return (Category == "All sources" || Category == category) &&
            (string.IsNullOrWhiteSpace(Query) ||
             $"{input.SlotNumber:00} {input.DisplayName} {input.SelectedUnifiedSourceLabel} {input.KindLabel}"
                 .Contains(Query.Trim(), StringComparison.OrdinalIgnoreCase));
    }
}
