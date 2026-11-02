using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Automation;
using Microsoft.UI.Xaml.Controls;

namespace CoreVideoPro.WinUI.Controls;

/// <summary>Graphics Designer slice 1 in the workspace: an open design is the private draft; the
/// inspector hosts its Variables panel; Save design stores a new version through the shared
/// GraphicsDesignService (a look that is not a design is saved as a new design copy); Save to
/// playlist also pins a playlist row on the next free F-key. Results and refusals show in the
/// design status line.</summary>
public sealed partial class GraphicsWorkspaceControl
{
    private readonly GraphicsVariablesPanel _variables = new();
    private Expander _variablesDrawer = null!;
    private readonly StackPanel _designActions = new() { Spacing = 6 };
    private readonly Button _saveDesign = ChromeButton("Save design"), _saveToPlaylist = ChromeButton("Save to playlist");
    private readonly TextBlock _designStatus = new() { FontSize = 14, TextWrapping = TextWrapping.Wrap };
    private bool _designSaving;

    private void BuildDesignActions(StackPanel host)
    {
        _variablesDrawer = new Expander { Header = "Variables", Content = _variables, IsExpanded = true, HorizontalAlignment = HorizontalAlignment.Stretch, HorizontalContentAlignment = HorizontalAlignment.Stretch };
        AutomationProperties.SetName(_variablesDrawer, "Design variables");
        var row = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 8 };
        foreach (var button in new[] { _saveDesign, _saveToPlaylist }) { button.FontSize = 14; row.Children.Add(button); }
        _saveDesign.Background = Brush(0x91, 0xE3, 0xBC); _saveDesign.Foreground = Brush(0x10, 0x23, 0x1A);
        AutomationProperties.SetName(_saveDesign, "Save design"); AutomationProperties.SetName(_saveToPlaylist, "Save to playlist");
        ToolTipService.SetToolTip(_saveDesign, "Store your edits as the design's next version. Program and Preview are unchanged.");
        ToolTipService.SetToolTip(_saveToPlaylist, "Save, then put this design in the playlist on the next free F-key. Show it from Operate.");
        _saveDesign.Click += async (_, _) => await SaveDesignAsync(toPlaylist: false);
        _saveToPlaylist.Click += async (_, _) => await SaveDesignAsync(toPlaylist: true);
        _layersEditor.DesignSaveRequested += async (_, _) => await SaveDesignAsync(toPlaylist: false);
        _designActions.Children.Add(row); _designActions.Children.Add(_designStatus);
        _designActions.Visibility = _variablesDrawer.Visibility = Visibility.Collapsed;
        host.Children.Add(_designActions);
    }

    private void OnDesignOpened(object? sender, EventArgs e)
    {
        RememberStarterEdits();
        _activeStarter = null; _starterHomeOpen = false; _starterInitialSelectionApplied = true; _tools.IsPaneOpen = false;
        _designStatus.Text = "Design open. Your edits stay private until you Save design.";
        _inspectorKey = ""; Refresh(); SelectInspectorTab(0);
    }

    private void RefreshDesignChrome()
    {
        var draft = _session?.Draft;
        var design = Studio?.GraphicsDesignFor(draft);
        var editable = draft is { Declarative: true } && _activeStarter is null && !_starterHomeOpen;
        var visible = editable && (design is not null || _advancedGraphicsControls) ? Visibility.Visible : Visibility.Collapsed;
        _designActions.Visibility = _variablesDrawer.Visibility = visible;
        _saveDesign.Content = _designSaving ? "Saving…" : design is null ? "Save as new design" : "Save design";
        _saveDesign.IsEnabled = _saveToPlaylist.IsEnabled = !_designSaving && _session is { Busy: false };
        if (visible == Visibility.Visible) _variables.Refresh();
    }

    private async Task SaveDesignAsync(bool toPlaylist)
    {
        if (Studio is not { } studio || _designSaving) return;
        foreach (var binding in _textFields.ToArray()) binding.CommitPending();
        _designSaving = true; _designStatus.Text = toPlaylist ? "Saving the design and adding it to the playlist…" : "Saving the design…"; RefreshDesignChrome();
        try {
            var result = toPlaylist ? await studio.SaveGraphicsDesignToPlaylistAsync() : await studio.SaveGraphicsDesignAsync();
            _designStatus.Text = result.Message;
        } catch (Exception ex) when (ex is not OutOfMemoryException) { _designStatus.Text = "The design was not saved: " + ex.Message; }
        finally { _designSaving = false; RefreshDesignChrome(); }
    }
}
