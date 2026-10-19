using CoreVideoPro.MediaCore.Models;
using CoreVideoPro.MediaCore.Services;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Automation;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Controls.Primitives;
using Microsoft.UI.Xaml.Media;

namespace CoreVideoPro.WinUI.Controls;

public sealed partial class GraphicsWorkspaceControl
{
    public event EventHandler? ExistingControlsRequested;
    private readonly StackPanel _contentControls = new() { Spacing = 12 }, _styleControls = new() { Spacing = 12 },
        _layoutControls = new() { Spacing = 12 }, _motionControls = new() { Spacing = 12 };
    private StackPanel _inspectorTarget = null!;
    private readonly SplitView _tools = new() { DisplayMode = SplitViewDisplayMode.Overlay, PanePlacement = SplitViewPanePlacement.Right, OpenPaneLength = 560, IsPaneOpen = false };
    private readonly ScrollViewer _toolScroll = new() { VerticalScrollBarVisibility = ScrollBarVisibility.Auto, HorizontalScrollBarVisibility = ScrollBarVisibility.Disabled };
    private readonly TextBlock _toolTitle = new() { FontSize = 18 }, _selectionTitle = new() { FontSize = 16 },
        _selectionSummary = new() { FontSize = 11, TextTrimming = TextTrimming.CharacterEllipsis },
        _assignmentCaption = new() { FontSize = 11, TextTrimming = TextTrimming.CharacterEllipsis },
        _layerHeading = new() { Text = "LAYERS · top is front", FontSize = 11 };
    private readonly Dictionary<string, FrameworkElement> _toolPanels = new(StringComparer.Ordinal);
    private readonly CheckBox _studioFollowSource = new() { Content = "Use Program source text in Studio", IsChecked = true };
    private readonly TextBox _cueSearch = new() { PlaceholderText = "Find a cue" };
    private readonly ComboBox _cueScopeFilter = new() { Header="Scope", ItemsSource=new[] {"All scenes","Current Program scene"}, SelectedIndex=0, HorizontalAlignment=HorizontalAlignment.Stretch };
    private readonly ListView _layerList = new() { SelectionMode = ListViewSelectionMode.Single };
    private IReadOnlyList<GraphicsEditorLayer> _layerRows = [];
    private string _layerStructure = "";
    private readonly Grid _inspectorBodies = new();
    private readonly List<Button> _inspectorTabs = [];
    private readonly List<ScrollViewer> _inspectorScrolls = [];
    private int _activeInspectorTab;
    private bool _advancedGraphicsControls;
    private readonly Button _customizeGraphic = ChromeButton("Customize layout & motion");
    private readonly RowDefinition _layerPanelRow = new() { Height = new GridLength(0) };
    private Button _useInStudioButton = null!;
    private readonly Grid _transport = new() { ColumnSpacing = 8, Padding = new Thickness(12, 8, 12, 8) };

    private void BuildWorkspace()
    {
        _inspectorTarget = _contentControls;
        _body.RowDefinitions.Add(new() { Height = new GridLength(1, GridUnitType.Star) });
        _body.ColumnDefinitions.Add(new() { Width = new GridLength(270) });
        _body.ColumnDefinitions.Add(new() { Width = new GridLength(1, GridUnitType.Star) });
        _body.ColumnDefinitions.Add(new() { Width = new GridLength(340) });

        var root = new Grid { RowSpacing = 10, Background = Brush(0x10,0x14,0x18) };
        root.RowDefinitions.Add(new() { Height = GridLength.Auto });
        root.RowDefinitions.Add(new() { Height = new GridLength(1, GridUnitType.Star) });
        root.RowDefinitions.Add(new() { Height = GridLength.Auto });
        var heading = new Grid { ColumnSpacing = 12 };
        heading.ColumnDefinitions.Add(new() { Width = new GridLength(1, GridUnitType.Star) });
        heading.ColumnDefinitions.Add(new() { Width = GridLength.Auto });
        heading.Children.Add(new TextBlock { Text = "Graphics", FontSize = 24, VerticalAlignment = VerticalAlignment.Center });
        var headerActions = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 8 };
        headerActions.Children.Add(Action("Show templates", () => { OpenShowTemplates(); return Task.CompletedTask; }));
        headerActions.Children.Add(Action("Automation", () => { OpenTools("automation", "Graphics automation"); return Task.CompletedTask; }));
        headerActions.Children.Add(Action("＋ Add graphic", () => { OpenTools("library", "Templates and saved cues"); return Task.CompletedTask; }));
        headerActions.Children.Add(Action("Manage", () => { OpenTools("manage", "Styles and cue management"); return Task.CompletedTask; }));
        headerActions.Children.Add(Action("Help", () => { OpenTools("help", "Graphics workflow"); return Task.CompletedTask; }));
        Grid.SetColumn(headerActions, 1); heading.Children.Add(headerActions); root.Children.Add(heading);

        var rack = new Grid { RowSpacing = 10, Padding = new Thickness(12) };
        rack.RowDefinitions.Add(new() { Height = GridLength.Auto }); rack.RowDefinitions.Add(new() { Height = GridLength.Auto }); rack.RowDefinitions.Add(new() { Height = new GridLength(1, GridUnitType.Star) }); rack.RowDefinitions.Add(new() { Height = GridLength.Auto });
        rack.Children.Add(new TextBlock { Text = "Show cues", FontSize = 16 });
        _cues.MaxHeight = double.PositiveInfinity; _cues.HorizontalContentAlignment = HorizontalAlignment.Stretch;
        Grid.SetRow(_cueSearch,1); rack.Children.Add(_cueSearch);
        _cueSearch.TextChanged += (_,_) => Refresh(); _cueScopeFilter.SelectionChanged += (_,_) => Refresh();
        AutomationProperties.SetName(_cues, "Graphics show cues"); Grid.SetRow(_cues, 2); rack.Children.Add(_cues);
        Grid.SetRow(_starterGraphics, 2); rack.Children.Add(_starterGraphics);
        var browse = Action("Templates and saved cues", () => { OpenTools("library", "Templates and saved cues"); return Task.CompletedTask; });
        var cueFooter = new StackPanel { Spacing=8 }; cueFooter.Children.Add(_cueScopeFilter); cueFooter.Children.Add(browse); Grid.SetRow(cueFooter, 3); rack.Children.Add(cueFooter); _body.Children.Add(Panel(rack));

        var center = new Grid { RowSpacing = 8 };
        center.RowDefinitions.Add(new() { Height = GridLength.Auto });
        center.RowDefinitions.Add(new() { Height = new GridLength(1, GridUnitType.Star) });
        center.RowDefinitions.Add(new() { Height = GridLength.Auto });
        center.RowDefinitions.Add(_layerPanelRow);
        var previewBar = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 10 };
        previewBar.Children.Add(new TextBlock { Text = "PRIVATE PREVIEW", FontSize = 11, VerticalAlignment = VerticalAlignment.Center });
        _samples.Header = null; _samples.Width = 150; _compare.Header = null; _compare.OnContent = _compare.OffContent = "Compare";
        previewBar.Children.Add(_samples); previewBar.Children.Add(_compare);
        var whole = new ToggleButton { Content = "Move graphic" }; whole.Checked += (_, _) => _canvas.WholeGraphic = true; whole.Unchecked += (_, _) => _canvas.WholeGraphic = false;
        previewBar.Children.Add(whole);
        var previewHeading = new StackPanel { Spacing = 8 }; previewHeading.Children.Add(previewBar); previewHeading.Children.Add(_starterBanner); previewHeading.Children.Add(_starterBrandTools); previewHeading.Children.Add(_starterFeedback); center.Children.Add(previewHeading);
        var canvasPanel = Panel(_canvas); Grid.SetRow(canvasPanel, 1); center.Children.Add(canvasPanel);
        var previewStatus = new Grid(); previewStatus.ColumnDefinitions.Add(new() { Width = new GridLength(1, GridUnitType.Star) }); previewStatus.ColumnDefinitions.Add(new() { Width = GridLength.Auto });
        _native.TextTrimming = TextTrimming.CharacterEllipsis; _native.TextWrapping = TextWrapping.NoWrap; previewStatus.Children.Add(_native);
        var editLayers = Action("Edit layers ↗", () => { OpenTools("layers", "Template layer editor"); return Task.CompletedTask; }); Grid.SetColumn(editLayers, 1); previewStatus.Children.Add(editLayers);
        Grid.SetRow(previewStatus, 2); center.Children.Add(previewStatus);
        var layerPanel = new Grid { RowSpacing = 4 }; layerPanel.RowDefinitions.Add(new() { Height = GridLength.Auto }); layerPanel.RowDefinitions.Add(new() { Height = new GridLength(1, GridUnitType.Star) });
        layerPanel.Children.Add(_layerHeading); Grid.SetRow(_layerList, 1); layerPanel.Children.Add(_layerList); Grid.SetRow(layerPanel, 3); center.Children.Add(layerPanel);
        _layerList.SelectionChanged += (_, _) => { if (!_loading && _layerList.SelectedIndex >= 0 && _layerList.SelectedIndex < _layerRows.Count) _session?.SelectLayer(_layerRows[_layerList.SelectedIndex].Id); };
        Grid.SetColumn(center, 1); _body.Children.Add(center);

        var inspector = new Grid { RowSpacing = 8, Padding = new Thickness(12) };
        foreach (var height in new[] { GridLength.Auto, GridLength.Auto, new GridLength(1, GridUnitType.Star), GridLength.Auto, GridLength.Auto }) inspector.RowDefinitions.Add(new() { Height = height });
        var selection = new StackPanel { Spacing = 4 }; selection.Children.Add(_selectionTitle); selection.Children.Add(_selectionSummary); selection.Children.Add(_customizeGraphic); inspector.Children.Add(selection);
        _customizeGraphic.Click += (_, _) => { SetGraphicsCustomization(!_advancedGraphicsControls); };
        var tabs = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 4 };
        var panels = new[] { _contentControls, _styleControls, _layoutControls, _motionControls };
        foreach (var (name, index) in new[] { ("Content", 0), ("Style", 1), ("Layout", 2), ("Motion", 3) }) {
            var button = ChromeButton(name); button.Padding = new Thickness(9, 6, 9, 6); button.Click += (_, _) => SelectInspectorTab(index); tabs.Children.Add(button); _inspectorTabs.Add(button);
            var scroll = new ScrollViewer { Content = panels[index], VerticalScrollBarVisibility = ScrollBarVisibility.Auto, HorizontalScrollBarVisibility = ScrollBarVisibility.Disabled };
            _inspectorBodies.Children.Add(scroll); _inspectorScrolls.Add(scroll);
        }
        Grid.SetRow(tabs, 1); inspector.Children.Add(tabs); Grid.SetRow(_inspectorBodies, 2); inspector.Children.Add(_inspectorBodies); SelectInspectorTab(0);
        var assignment = new StackPanel { Spacing = 4 };
        assignment.Children.Add(_assignmentCaption);
        _useInStudioButton = Action("Use in Studio", async () => { if (Studio is not null) await Studio.AssignGraphicsLookToStudioAsync(_studioFollowSource.IsChecked == true); }); assignment.Children.Add(_useInStudioButton);
        assignment.Children.Add(_assignmentHelp);
        ToolTipService.SetToolTip(_useInStudioButton, "Pin this draft for Studio's next Show. Current Program is unchanged.");
        Grid.SetRow(assignment, 3); inspector.Children.Add(assignment);
        _undoButton = Action("Undo draft", () => { _session?.Undo(); return Task.CompletedTask; }); Grid.SetRow(_undoButton, 4); inspector.Children.Add(_undoButton);
        var inspectorBorder = Panel(inspector); Grid.SetColumn(inspectorBorder, 2); _body.Children.Add(inspectorBorder);
        Grid.SetRow(_body, 1); root.Children.Add(_body);

        _transport.ColumnDefinitions.Add(new() { Width = new GridLength(1, GridUnitType.Star) });
        _status.TextWrapping = TextWrapping.NoWrap; _status.TextTrimming = TextTrimming.CharacterEllipsis; _status.VerticalAlignment = VerticalAlignment.Center; _transport.Children.Add(_status);
        _cueButton = Action("Cue to Preview", async () => { if (_session is not null) await _session.CueAsync(); });
        _showButton = Action("Show on Program", async () => { if (_session is not null) await _session.ShowAsync(); });
        _hideButton = Action("Hide", async () => { if (_session is not null) await _session.HideAsync(); });
        _updateButton = Action("Update on air", async () => { if (_session is not null) await _session.UpdateProgramAsync(); });
        var cancel = Action("Cancel draft", () => { _session?.CancelDraft(); return Task.CompletedTask; });
        var clear = Action("Clear graphics", async () => { if (_session is not null) await _session.ClearAsync(); });
        _showButton.Background = Brush(0x91,0xE3,0xBC); _showButton.Foreground = Brush(0x10,0x23,0x1A);
        clear.Foreground = Brush(0xFF,0x99,0x99); clear.Background = Brush(0x32,0x20,0x24); clear.BorderBrush = Brush(0x63,0x40,0x46);
        foreach (var button in new[] { _cueButton, _showButton, _hideButton, _updateButton, cancel, clear }) { _transport.ColumnDefinitions.Add(new() { Width = GridLength.Auto }); Grid.SetColumn(button, _transport.ColumnDefinitions.Count - 1); _transport.Children.Add(button); }
        var footer = Panel(_transport); Grid.SetRow(footer, 2); root.Children.Add(footer);

        BuildLibraryTools(); BuildCueManagement();
        _toolPanels["library"] = _libraryPanel;
        var manage = new StackPanel { Spacing = 10 };
        manage.Children.Add(_styles); manage.Children.Add(_styleName);
        manage.Children.Add(Action("Save style preset", async () => { if (_savedLibrary is not null) await _savedLibrary.SaveStyleAsync(_styleName.Text); }));
        manage.Children.Add(Action("Use selected saved style in draft", () => { if (_styles.SelectedItem is string name) _savedLibrary?.ApplyStyle(name); return Task.CompletedTask; }));
        manage.Children.Add(Action("Use current brand look in draft", () => { if (Studio is not null) _session?.SetStyle(Studio.Overlays.AppliedAppearance); return Task.CompletedTask; }));
        manage.Children.Add(Action("Cue and package management", () => { OpenTools("library", "Templates, saved cues and packages"); return Task.CompletedTask; }));
        manage.Children.Add(Action("Existing overlay controls", () => { _tools.IsPaneOpen = false; ExistingControlsRequested?.Invoke(this, EventArgs.Empty); return Task.CompletedTask; }));
        manage.Children.Add(Action("Remove Studio assignment", async () => { if (_savedLibrary is not null) await _savedLibrary.ClearStudioLowerThirdAsync(); }));
        _toolPanels["manage"] = manage; _toolPanels["automation"] = _automationDrawer; _automationDrawer.IsExpanded = true;
        _toolPanels["layers"] = _layersEditor;
        var help = new StackPanel { Spacing = 12 };
        help.Children.Add(Copy("Choose or create a cue. Content, Style, Layout and Motion edit a private draft. Cue prepares Preview; Show reveals a prepared cue; Update changes an on-air graphic. Hide affects the selected graphic; Clear removes managed graphics and inhibits automation."));
        help.Children.Add(Copy("Use in Studio saves an independent copy for the existing Studio lower-third toggle. Browsing or editing another cue never changes that assignment. Reassigning while on air takes effect at the next Show. Loading an assignment never starts an output or arms automation."));
        help.Children.Add(Copy("Native revision and resource details")); help.Children.Add(_facts); _toolPanels["help"] = help;
        BuildStarters(root); SetGraphicsCustomization(false);
        var pane = new Grid { Padding = new Thickness(16), RowSpacing = 12 }; pane.RowDefinitions.Add(new() { Height = GridLength.Auto }); pane.RowDefinitions.Add(new() { Height = new GridLength(1, GridUnitType.Star) });
        var drawerHeader = new Grid(); drawerHeader.ColumnDefinitions.Add(new() { Width = new GridLength(1, GridUnitType.Star) }); drawerHeader.ColumnDefinitions.Add(new() { Width = GridLength.Auto }); drawerHeader.Children.Add(_toolTitle);
        var close = ChromeButton("Close"); close.Click += (_, _) => _tools.IsPaneOpen = false; Grid.SetColumn(close, 1); drawerHeader.Children.Add(close); pane.Children.Add(drawerHeader);
        Grid.SetRow(_toolScroll, 1); pane.Children.Add(_toolScroll); _tools.Pane = Panel(pane); _tools.Content = root; Content = _tools;
    }

    private static SolidColorBrush Brush(byte r, byte g, byte b) => new(Windows.UI.Color.FromArgb(255,r,g,b));
    private static Button ChromeButton(string label) => new() { Content=label, FontSize=13, Padding=new Thickness(12,8,12,8), CornerRadius=new CornerRadius(6), Background=Brush(0x20,0x28,0x30), Foreground=Brush(0xED,0xF2,0xF7), BorderBrush=Brush(0x30,0x3B,0x46), BorderThickness=new Thickness(1) };
    private static Border Panel(UIElement content) => new() { Child = content, Background=Brush(0x17,0x1D,0x23), BorderThickness = new Thickness(1), CornerRadius = new CornerRadius(7), BorderBrush = new SolidColorBrush(Windows.UI.Color.FromArgb(255, 48, 59, 70)) };
    private void OpenTools(string key, string title) { _toolTitle.Text = title; _toolScroll.Content = null; _toolScroll.Content = _toolPanels[key]; _tools.IsPaneOpen = true; }
    private void SelectInspectorTab(int index)
    {
        _activeInspectorTab = index;
        for (var i = 0; i < _inspectorScrolls.Count; i++) { _inspectorScrolls[i].Visibility = i == index ? Visibility.Visible : Visibility.Collapsed; _inspectorTabs[i].Opacity = i == index ? 1 : .65; }
    }
    private void SetGraphicsCustomization(bool advanced)
    {
        _advancedGraphicsControls = advanced;
        _customizeGraphic.Content = advanced ? "Use simple controls" : "Customize layout & motion";
        for (var i = 2; i < _inspectorTabs.Count; ++i) _inspectorTabs[i].Visibility = advanced ? Visibility.Visible : Visibility.Collapsed;
        _layerPanelRow.Height = new GridLength(advanced ? 118 : 0);
        _layerHeading.Visibility = _layerList.Visibility = advanced ? Visibility.Visible : Visibility.Collapsed;
        if (!advanced) SelectInspectorTab(0);
    }
    private void ApplyWorkspaceSize(double width)
    {
        // Keep the selected inspector beside the work, never below an unbounded
        // library. Only secondary tools overlay when requested.
        _body.ColumnDefinitions[0].Width = new GridLength(width < 1300 ? 220 : 270);
        _body.ColumnDefinitions[2].Width = new GridLength(width < 1300 ? 300 : 340);
        _tools.OpenPaneLength = Math.Min(560, Math.Max(280, width - 24));
    }
    private void RefreshWorkspaceChrome()
    {
        _selectionTitle.Text = _activeStarter is not null && _starterPreviewIndex>=0 && _starterPreviewIndex<_starterDrafts.Count ? _starterDrafts[_starterPreviewIndex].Name : _session?.Selected?.Label is { Length: > 0 } name ? name : _session?.Template?.Name ?? "Choose a graphic";
        _selectionSummary.Text = _activeStarter is not null ? "Private sample · not added to your show" : _session?.Draft is not null ? (_session.Dirty ? "Private edits · not applied" : "Your graphic") : "Choose a show template or an existing graphic";
        _assignmentCaption.Text = "Studio look: " + (Studio?.StudioAssignedLowerThirdLabel ?? "Not assigned");
        _useInStudioButton.IsEnabled = _session is { Busy: false, Selected: not null, Draft: not null, PreviewSample: "current" } && _session.Template?.Family == "lower-third" && Studio?.StudioLowerThirdBusy == false;
    }
    private void RefreshLayerList()
    {
        var document = _session?.TemplateDocument;
        var authored = document is null ? Array.Empty<GraphicsEditorLayer>() : GraphicsTemplateEditor.Layers(document).ToArray();
        IEnumerable<GraphicsEditorLayer> FrontToBack(string parent) {
            foreach (var layer in authored.Where(l => l.Parent == parent).Reverse()) { yield return layer; foreach (var child in FrontToBack(layer.Id)) yield return child; }
        }
        var rows = FrontToBack("").ToArray();
        var signature = string.Join('|', rows.Select(r => r.Id + ":" + r.Label + ":" + r.Visible + ":" + r.Locked));
        if (signature != _layerStructure) { _layerStructure = signature; _layerRows = rows; _layerList.Items.Clear(); foreach (var row in rows) _layerList.Items.Add($"{(row.Visible ? "◉" : "○")}  {row.Label} · {row.Type}{(row.Locked ? " · locked" : "")}"); }
        _layerList.SelectedIndex = _layerRows.ToList().FindIndex(r => r.Id == _session?.SelectedLayerId);
    }
    private void BuildMotionControls()
    {
        _inspectorTarget = _motionControls;
        foreach (var phase in new[] { "in", "out" }) {
            var transition = phase == "in" ? _session!.Draft!.Animations.In : _session!.Draft!.Animations.Out;
            var kind = new ComboBox { Header = phase == "in" ? "Animate in" : "Animate out", ItemsSource = new[] { "none", "fade", "slide" }, SelectedItem = transition.Type, HorizontalAlignment = HorizontalAlignment.Stretch };
            var direction = new ComboBox { Header = "Direction", ItemsSource = new[] { "left", "right", "up", "down" }, SelectedItem = transition.Direction, HorizontalAlignment = HorizontalAlignment.Stretch };
            var duration = new SliderValueControl { Header = "Duration", Unit = "ms", Minimum = 0, Maximum = 2000, DefaultValue = phase == "in" ? 250 : 200, StepFrequency = 25, Value = transition.DurationMs };
            void Edit() { if (_loading || _session?.Draft is not { } d) return; var value = (phase == "in" ? d.Animations.In : d.Animations.Out) with { Type = kind.SelectedItem as string ?? "fade", Direction = direction.SelectedItem as string ?? "down", DurationMs = (int)duration.Value }; _session.Edit(d with { Animations = phase == "in" ? d.Animations with { In = value } : d.Animations with { Out = value } }); }
            kind.SelectionChanged += (_, _) => Edit(); direction.SelectionChanged += (_, _) => Edit(); duration.ValueChanged += (_, _) => Edit(); duration.EditStarted += (_, _) => _session?.BeginGesture(); duration.EditCompleted += (_, _) => _session?.EndGesture();
            _motionControls.Children.Add(kind); _motionControls.Children.Add(direction); _motionControls.Children.Add(duration);
            _refreshFields.Add(() => { if (_session?.Draft is not { } d) return; var current = phase == "in" ? d.Animations.In : d.Animations.Out; kind.SelectedItem = current.Type; direction.SelectedItem = current.Direction; duration.Value = current.DurationMs; });
        }
    }
}
