using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;

namespace CoreVideoPro.WinUI.Controls;

/// <summary>Designs in "Cues &amp; automation" (Graphics Designer slice 2): designs are edited in the Designer,
/// not here. A design draft is never a Studio cue, so its Studio-look rows collapse while one is shown.</summary>
public sealed partial class GraphicsWorkspaceControl
{
    private readonly StackPanel _assignmentPanel = new() { Spacing = 4 };

    private void RefreshDesignChrome()
    {
        var draft = _session?.Draft;
        var design = Studio?.GraphicsDesignFor(draft);
        var showsDesign = design is not null && draft is { Declarative: true } && _activeStarter is null && !_starterHomeOpen;
        _assignmentPanel.Visibility = showsDesign ? Visibility.Collapsed : Visibility.Visible;
    }
}
