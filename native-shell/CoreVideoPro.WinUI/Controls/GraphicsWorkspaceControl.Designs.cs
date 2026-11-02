using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Automation;
using Microsoft.UI.Xaml.Controls;

namespace CoreVideoPro.WinUI.Controls;

/// <summary>Graphics Designer slice 1 in the workspace: an open design is the private draft. Save design /
/// Save to playlist and their status sit in the workspace header (they cost the inspector no height);
/// the Variables panel is its own inspector tab with the full inspector body; the Studio-look rows
/// (meaningless for a design draft) collapse while a design is edited. Editing is paused while a save
/// is in flight so nothing typed meanwhile is dropped when the draft re-opens on the saved version.</summary>
public sealed partial class GraphicsWorkspaceControl
{
    private const int VariablesTab = 4;
    private readonly GraphicsVariablesPanel _variables = new();
    private readonly StackPanel _assignmentPanel = new() { Spacing = 4 };
    private readonly StackPanel _designActions = new() { Orientation = Orientation.Horizontal, Spacing = 8, VerticalAlignment = VerticalAlignment.Center };
    private readonly Button _saveDesign = ChromeButton("Save design"), _saveToPlaylist = ChromeButton("Save to playlist");
    private Button _variablesTab = null!;
    private readonly TextBlock _designStatus = new() { FontSize = 14, TextWrapping = TextWrapping.Wrap, MaxLines = 2, TextTrimming = TextTrimming.CharacterEllipsis, VerticalAlignment = VerticalAlignment.Center };
    private bool _designSaving;

    private void BuildDesignActions(Grid titleRow)
    {
        foreach (var button in new[] { _saveDesign, _saveToPlaylist }) { button.FontSize = 14; _designActions.Children.Add(button); }
        _saveDesign.Background = Brush(0x91, 0xE3, 0xBC); _saveDesign.Foreground = Brush(0x10, 0x23, 0x1A);
        AutomationProperties.SetName(_saveDesign, "Save design"); AutomationProperties.SetName(_saveToPlaylist, "Save to playlist");
        AutomationProperties.SetName(_designStatus, "Design status");
        ToolTipService.SetToolTip(_saveDesign, "Store your edits as the design's next version. Program and Preview are unchanged.");
        ToolTipService.SetToolTip(_saveToPlaylist, "Save, then put this design in the playlist on the next free F-key. Show it from Operate.");
        _saveDesign.Click += async (_, _) => await SaveDesignAsync(toPlaylist: false);
        _saveToPlaylist.Click += async (_, _) => await SaveDesignAsync(toPlaylist: true);
        _layersEditor.DesignSaveRequested += async (_, _) => await SaveDesignAsync(toPlaylist: false);
        _designActions.Visibility = _designStatus.Visibility = Visibility.Collapsed;
        Grid.SetColumn(_designActions, 1); titleRow.Children.Add(_designActions);
        Grid.SetColumn(_designStatus, 2); titleRow.Children.Add(_designStatus);
    }

    private void AddVariablesTab(StackPanel tabs)
    {
        _variablesTab = ChromeButton("Variables"); _variablesTab.Padding = new Thickness(9, 6, 9, 6);
        _variablesTab.Click += (_, _) => SelectInspectorTab(VariablesTab); _variablesTab.Visibility = Visibility.Collapsed;
        tabs.Children.Add(_variablesTab); _inspectorTabs.Add(_variablesTab);
        var scroll = new ScrollViewer { Content = _variables, VerticalScrollBarVisibility = ScrollBarVisibility.Auto, HorizontalScrollBarVisibility = ScrollBarVisibility.Disabled };
        AutomationProperties.SetName(scroll, "Design variables");
        _inspectorBodies.Children.Add(scroll); _inspectorScrolls.Add(scroll);
    }

    private void OnDesignOpened(object? sender, EventArgs e)
    {
        RememberStarterEdits();
        _activeStarter = null; _starterHomeOpen = false; _starterInitialSelectionApplied = true; _tools.IsPaneOpen = false;
        _designStatus.Text = "Design open. Your edits stay private until you Save design.";
        _inspectorKey = ""; Refresh(); SelectInspectorTab(VariablesTab);
    }

    private void RefreshDesignChrome()
    {
        var draft = _session?.Draft;
        var design = Studio?.GraphicsDesignFor(draft);
        var editable = draft is { Declarative: true } && _activeStarter is null && !_starterHomeOpen;
        var visible = editable && (design is not null || _advancedGraphicsControls);
        _designActions.Visibility = _variablesTab.Visibility = visible ? Visibility.Visible : Visibility.Collapsed;
        _designStatus.Visibility = visible && _designStatus.Text.Length > 0 ? Visibility.Visible : Visibility.Collapsed;
        ToolTipService.SetToolTip(_designStatus, _designStatus.Text);
        // A design draft is never a Studio cue; its Studio-look rows would only take the Variables tab's height.
        _assignmentPanel.Visibility = visible && design is not null ? Visibility.Collapsed : Visibility.Visible;
        if (!visible && _activeInspectorTab == VariablesTab) SelectInspectorTab(0);
        _saveDesign.Content = _designSaving ? "Saving…" : design is null ? "Save as new design" : "Save design";
        _saveDesign.IsEnabled = _saveToPlaylist.IsEnabled = !_designSaving && _session is { Busy: false };
        // Pause editing during a save: the save captured the manifest, and the draft re-opens on the new version.
        foreach (var scroll in _inspectorScrolls) scroll.IsEnabled = !_starterAdding && !_designSaving;
        _layersEditor.IsEnabled = _layerList.IsEnabled = !_designSaving;
        _canvas.IsEnabled = !_starterAdding && !_designSaving;
        if (_designSaving) { _undoButton.IsEnabled = false; foreach (var button in _inspectorActions) button.IsEnabled = false; }
        if (visible) _variables.Refresh();
    }

    private async Task SaveDesignAsync(bool toPlaylist)
    {
        if (Studio is not { } studio || _designSaving) return;
        foreach (var binding in _textFields.ToArray()) binding.CommitPending();
        _designSaving = true;
        _designStatus.Text = (toPlaylist ? "Saving the design and adding it to the playlist…" : "Saving the design…") + " Editing is paused until it finishes.";
        Refresh();
        try {
            var result = toPlaylist ? await studio.SaveGraphicsDesignToPlaylistAsync() : await studio.SaveGraphicsDesignAsync();
            _designStatus.Text = result.Message;
        } catch (Exception ex) when (ex is not OutOfMemoryException) { _designStatus.Text = "The design was not saved: " + ex.Message; }
        finally { _designSaving = false; Refresh(); }
    }
}
