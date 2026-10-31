using System.Text.Json;
using CoreVideoPro.MediaCore.Models;
using CoreVideoPro.MediaCore.Services;
using CoreVideoPro.WinUI.Models;
using CoreVideoPro.WinUI.Services;
using CoreVideoPro.WinUI.ViewModels;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Input;
using Microsoft.UI.Xaml.Media;
using Microsoft.UI.Xaml.Shapes;

namespace CoreVideoPro.WinUI.Controls;

/// <summary>The graphics playlist (issue #1076): one row per <see cref="GraphicsPlaylistRow"/>,
/// the Guest row expanded into its roster entries, with Show / Cue / Hide /
/// Remove per item. Full form adds the inspector; <see cref="Compact"/> (Studio right panel)
/// shows rows and entries with Show / Hide only. Row state comes from the native strip,
/// never from the button that was pressed.</summary>
public sealed class GraphicsPlayoutControl : UserControl
{
    public static readonly DependencyProperty StudioProperty = DependencyProperty.Register(nameof(Studio), typeof(StudioViewModel),
        typeof(GraphicsPlayoutControl), new PropertyMetadata(null, (d, _) => ((GraphicsPlayoutControl)d).Attach()));
    public static readonly DependencyProperty CompactProperty = DependencyProperty.Register(nameof(Compact), typeof(bool),
        typeof(GraphicsPlayoutControl), new PropertyMetadata(false, (d, _) => ((GraphicsPlayoutControl)d).ApplyCompact()));
    public StudioViewModel? Studio { get => (StudioViewModel?)GetValue(StudioProperty); set => SetValue(StudioProperty, value); }
    public bool Compact { get => (bool)GetValue(CompactProperty); set => SetValue(CompactProperty, value); }

    private const string DocsBase = "https://github.com/iamfatness/CoreVideoPro/blob/main/docs/reference/graphics-playout.md";
    private static readonly string[] HotkeyChoices = ["None", "F1", "F2", "F3", "F4", "F5", "F6", "F7", "F8", "F9", "F10", "F11", "F12"];
    private static readonly string[] LayoutHotkeyChoices = ["None", .. Enumerable.Range(1, 11).Select(n => ShotLayout.HotkeyPrefix + n)];
    private static readonly JsonSerializerOptions LayoutJson = new(JsonSerializerDefaults.Web);
    private static readonly (string Label, int Ms)[] PlayOutChoices = [("Manual", 0), ("8 s", 8000), ("15 s", 15000)];

    private StudioViewModel? _attached;
    private GraphicsWorkspaceLibrary? _library;
    private readonly Grid _root = new() { ColumnSpacing = 12 };
    private readonly ColumnDefinition _inspectorColumn = new() { Width = new GridLength(300) };
    private readonly StackPanel _list = new() { Spacing = 8 };
    private readonly StackPanel _layouts = new() { Spacing = 6 };
    private string _layoutsSignature = "", _selectedLayoutId = "";
    private readonly StackPanel _inspector = new() { Spacing = 10 };
    private readonly Border _inspectorPanel;
    private readonly TextBlock _status = new() { FontSize = 11, TextWrapping = TextWrapping.Wrap };
    private readonly Button _add;
    private string _selectedKey = "", _listSignature = "", _inspectorSignature = "";
    private bool _loading;

    public GraphicsPlayoutControl()
    {
        _root.ColumnDefinitions.Add(new() { Width = new GridLength(1, GridUnitType.Star) });
        _root.ColumnDefinitions.Add(_inspectorColumn);
        var left = new Grid { RowSpacing = 8 };
        left.RowDefinitions.Add(new() { Height = GridLength.Auto }); left.RowDefinitions.Add(new() { Height = GridLength.Auto });
        left.RowDefinitions.Add(new() { Height = new GridLength(1, GridUnitType.Star) }); left.RowDefinitions.Add(new() { Height = GridLength.Auto });
        var header = new Grid { ColumnSpacing = 8 };
        header.ColumnDefinitions.Add(new() { Width = new GridLength(1, GridUnitType.Star) }); header.ColumnDefinitions.Add(new() { Width = GridLength.Auto });
        header.Children.Add(new TextBlock { Text = "PLAYLIST", FontSize = 10, CharacterSpacing = 160, FontFamily = StudioBrushes.Mono, Foreground = StudioBrushes.Muted, VerticalAlignment = VerticalAlignment.Center });
        _add = StudioBrushes.GhostButton("＋ Add graphic"); _add.Click += async (_, _) => await AddGraphicAsync(); Grid.SetColumn(_add, 1); header.Children.Add(_add);
        ToolTipService.SetToolTip(_add, "Add a graphic. An empty playlist installs the default five rows; otherwise this opens Design.");
        left.Children.Add(header);
        // Shot layouts sit under the Preview/Program strip, above the playlist (full form only).
        Grid.SetRow(_layouts, 1); left.Children.Add(_layouts);
        var scroll = new ScrollViewer { Content = _list, VerticalScrollBarVisibility = ScrollBarVisibility.Auto, HorizontalScrollBarVisibility = ScrollBarVisibility.Disabled };
        Grid.SetRow(scroll, 2); left.Children.Add(scroll);
        _status.Foreground = StudioBrushes.Muted; Grid.SetRow(_status, 3); left.Children.Add(_status);
        _root.Children.Add(left);
        _inspectorPanel = new Border { Background = StudioBrushes.Panel, BorderBrush = StudioBrushes.Border, BorderThickness = new Thickness(1), CornerRadius = new CornerRadius(8), Padding = new Thickness(12),
            Child = new ScrollViewer { Content = _inspector, VerticalScrollBarVisibility = ScrollBarVisibility.Auto, HorizontalScrollBarVisibility = ScrollBarVisibility.Disabled } };
        Grid.SetColumn(_inspectorPanel, 1); _root.Children.Add(_inspectorPanel);
        Content = _root;
        ApplyCompact();
        Loaded += (_, _) => Attach(); Unloaded += (_, _) => Detach();
    }

    private void ApplyCompact()
    {
        _inspectorColumn.Width = Compact ? new GridLength(0) : new GridLength(300);
        _inspectorPanel.Visibility = Compact ? Visibility.Collapsed : Visibility.Visible;
        _layouts.Visibility = Compact ? Visibility.Collapsed : Visibility.Visible;
        _listSignature = ""; _layoutsSignature = ""; Refresh();
    }

    // ---- lifecycle -----------------------------------------------------------------------
    private void Attach()
    {
        if (Studio is null || !IsLoaded || ReferenceEquals(_attached, Studio)) return;
        Detach(); _attached = Studio; _attached.PropertyChanged += OnStudioProperty;
        _library = Studio.GraphicsLibrary; _library.Changed += OnLibraryChanged; _ = _library.LoadAsync();
        _listSignature = ""; _inspectorSignature = ""; _layoutsSignature = ""; Refresh();
    }
    private void Detach()
    {
        if (_attached is not null) _attached.PropertyChanged -= OnStudioProperty;
        if (_library is not null) _library.Changed -= OnLibraryChanged;
        _attached = null; _library = null;
    }
    private void OnLibraryChanged(object? sender, EventArgs e) => Refresh();
    private void OnStudioProperty(object? sender, System.ComponentModel.PropertyChangedEventArgs e)
    {
        if (e.PropertyName is nameof(StudioViewModel.GraphicsStrip) or nameof(StudioViewModel.GuestEntries) or nameof(StudioViewModel.GraphicsPlayoutStatus)
            or nameof(StudioViewModel.GraphicsPlayoutBusy) or nameof(StudioViewModel.GraphicsProgramSubjectSourceId) or nameof(StudioViewModel.PreviewSceneId)) Refresh();
    }

    // ---- actions -------------------------------------------------------------------------
    private async Task Invoke(string actionId, params object?[] args)
    {
        if (_attached is not { } studio) return;
        var result = await GraphicsPlayoutActions.InvokeAsync(studio, actionId, args);
        if (!result.Ok) _status.Text = result.Error ?? "Refused.";
        Refresh();
    }
    private async Task AddGraphicAsync()
    {
        if (_attached is not { } studio) return;
        if (studio.GraphicsPlayout.Playlist.Count == 0) await Invoke(GraphicsPlayoutActions.PlaylistDefault, GraphicsShowStarters.All[0].Id);
        else { studio.GraphicsMode = StudioViewModel.GraphicsModeDesign; studio.ActiveTab = StudioTab.Overlays; }
    }
    private async Task SaveRowAsync(string rowId, Func<GraphicsPlaylistRow, GraphicsPlaylistRow> update)
    {
        if (_attached is not { } studio) return;
        var rows = studio.GraphicsPlayout.Playlist.Select(r => r.Id == rowId ? update(r) : r).ToArray();
        if (!await studio.GraphicsLibrary.SavePlaylistAsync(rows)) _status.Text = studio.GraphicsLibrary.Status;
        Refresh();
    }
    private static GraphicsPlaylistRow WithField(GraphicsPlaylistRow row, string key, string value)
    {
        var fields = row.Fields.ToDictionary(p => p.Key, p => p.Value, StringComparer.Ordinal);
        fields[key] = JsonSerializer.SerializeToElement(value);
        return row with { Fields = fields };
    }
    private static string? TitleFieldKey(GraphicsPlaylistRow row) => row.Kind == GraphicsPlaylistRow.Guest ? "title" :
        new[] { "title", "subtitle", "label" }.FirstOrDefault(row.Fields.ContainsKey);
    private static string FieldText(GraphicsPlaylistRow row, string key) => row.Fields.TryGetValue(key, out var v) && v.ValueKind == JsonValueKind.String ? v.GetString() ?? "" : "";

    // ---- state ---------------------------------------------------------------------------
    private enum AirState { Off, Preview, Program }
    private AirState StateOf(string instanceId)
    {
        var strip = _attached?.GraphicsStrip ?? GraphicsStripState.Empty;
        if (strip.OnProgram.Any(i => i.InstanceId == instanceId)) return AirState.Program;
        return strip.OnPreview.Any(i => i.InstanceId == instanceId) ? AirState.Preview : AirState.Off;
    }
    private static Brush DotBrush(AirState state) => state switch { AirState.Program => StudioBrushes.Air, AirState.Preview => StudioBrushes.Program, _ => StudioBrushes.Dim };
    private static string Caption(GraphicsPlaylistRow row) => row.Kind switch {
        GraphicsPlaylistRow.Guest => row.FollowSources ? "Guest · names from Sources" : "Guest",
        GraphicsPlaylistRow.Title => "Title", GraphicsPlaylistRow.Logo => "Logo", GraphicsPlaylistRow.LiveBug => "Live bug",
        GraphicsPlaylistRow.Sponsor => row.TemplateId.Length == 0 && row.Image is null ? "Sponsor card · no image yet" : "Sponsor card", _ => row.Kind };

    // ---- rendering -----------------------------------------------------------------------
    public void Refresh()
    {
        if (_attached is not { } studio) return;
        var rows = studio.GraphicsPlayout.Playlist; var entries = studio.GuestEntries; var busy = studio.GraphicsPlayoutBusy;
        if (studio.GraphicsPlayoutStatus.Length > 0) _status.Text = studio.GraphicsPlayoutStatus;
        _add.IsEnabled = !busy && !studio.GraphicsLibrary.Busy;
        _add.Content = rows.Count == 0 ? "＋ Add graphic" : "＋ Add graphic ↗";
        var subject = studio.GraphicsProgramSubjectSourceId ?? "";
        var signature = $"{Compact}|{busy}|{_selectedKey}|{subject}|" +
            string.Join('|', rows.Select(r => $"{r.Id}:{r.Kind}:{r.Name}:{r.Hotkey}:{r.PlayOutAfterMs}:{r.FollowSources}:{r.TemplateId}:{StateOf(GraphicsPlayoutService.InstanceIdFor(r.Id, ""))}")) + "|" +
            string.Join('|', entries.Select(e => $"{e.RowId}:{e.SourceId}:{e.Label}:{e.Title}:{StateOf(GraphicsPlayoutService.InstanceIdFor(e))}"));
        if (signature != _listSignature) {
            _listSignature = signature; _list.Children.Clear();
            if (rows.Count == 0) _list.Children.Add(EmptyState(busy));
            foreach (var row in rows) _list.Children.Add(RowCard(row, entries.Where(e => e.RowId == row.Id).ToArray(), subject, busy));
        }
        if (!Compact) { RefreshLayouts(studio, rows, busy); RefreshInspector(rows, entries); }
    }

    // ---- shot layouts (#1076 slice 2) ---------------------------------------------------
    private static string SceneName(StudioViewModel studio, string sceneId) =>
        sceneId.Length == 0 ? "" : studio.Scenes.FirstOrDefault(s => s.Id == sceneId)?.Name ?? sceneId + " (missing)";
    private static string LayoutSummary(StudioViewModel studio, ShotLayout layout, IReadOnlyList<GraphicsPlaylistRow> rows)
    {
        var scene = layout.SceneId.Length == 0 ? "No scene change" : SceneName(studio, layout.SceneId);
        var names = layout.RowIds.Select(id => rows.FirstOrDefault(r => r.Id == id)?.Name ?? id + " (missing)").ToArray();
        var graphics = names.Length == 0 ? (layout.PlayOutOthers ? "plays everything out" : "no graphics") : string.Join(", ", names) + (layout.PlayOutOthers ? " · hides the rest" : "");
        return scene + " · " + graphics;
    }

    private void RefreshLayouts(StudioViewModel studio, IReadOnlyList<GraphicsPlaylistRow> rows, bool busy)
    {
        var layouts = studio.GraphicsPlayout.Layouts; var preview = studio.PreviewSceneId ?? "";
        var signature = $"{busy}|{_selectedLayoutId}|{preview}|{rows.Count}|" + string.Join('|', layouts.Select(l => $"{l.Id}:{l.Name}:{l.SceneId}:{string.Join(',', l.RowIds)}:{l.Hotkey}:{l.PlayOutOthers}:{LayoutSummary(studio, l, rows)}"));
        if (signature == _layoutsSignature) return;
        _layoutsSignature = signature; _layouts.Children.Clear();
        var header = new Grid { ColumnSpacing = 8 };
        header.ColumnDefinitions.Add(new() { Width = new GridLength(1, GridUnitType.Star) }); header.ColumnDefinitions.Add(new() { Width = GridLength.Auto }); header.ColumnDefinitions.Add(new() { Width = GridLength.Auto });
        header.Children.Add(new TextBlock { Text = "SHOT LAYOUTS", FontSize = 10, CharacterSpacing = 160, FontFamily = StudioBrushes.Mono, Foreground = StudioBrushes.Muted, VerticalAlignment = VerticalAlignment.Center });
        if (rows.Count == 0) {
            // First open: the layouts appear once the default playlist exists (its presets install with it).
            var note = new TextBlock { Text = "Appear once you have graphics.", FontSize = 11, Foreground = StudioBrushes.Muted, VerticalAlignment = VerticalAlignment.Center };
            Grid.SetColumn(note, 1); header.Children.Add(note); _layouts.Children.Add(header); return;
        }
        var selected = layouts.FirstOrDefault(l => l.Id == _selectedLayoutId);
        if (selected is not null) {
            var take = StudioBrushes.ChipButton("Cue and Take"); take.IsEnabled = !busy;
            ToolTipService.SetToolTip(take, $"Cue {selected.Name} to Preview, then Take: the picture and its graphics change together.");
            take.Click += async (_, _) => await Invoke(GraphicsPlayoutActions.LayoutTake, selected.Id);
            Grid.SetColumn(take, 1); header.Children.Add(take);
        }
        var edit = StudioBrushes.ChipButton("Edit layouts"); edit.IsEnabled = !busy;
        ToolTipService.SetToolTip(edit, "Pick a scene, tick the graphics and set a Shift+F key for each layout. Program and Preview are unchanged.");
        edit.Click += (_, _) => OpenLayoutEditor(edit, studio, rows);
        Grid.SetColumn(edit, 2); header.Children.Add(edit);
        _layouts.Children.Add(header);
        if (layouts.Count == 0) {
            _layouts.Children.Add(new TextBlock { Text = "No shot layouts yet. Edit layouts to make one: a scene plus the graphics that ride the Take with it.", FontSize = 11, Foreground = StudioBrushes.Muted, TextWrapping = TextWrapping.Wrap });
            return;
        }
        var strip = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 8 };
        foreach (var layout in layouts) strip.Children.Add(LayoutButton(studio, layout, rows, layout.Id == _selectedLayoutId, layout.SceneId.Length > 0 && layout.SceneId == preview, busy));
        _layouts.Children.Add(new ScrollViewer { Content = strip, HorizontalScrollBarVisibility = ScrollBarVisibility.Auto, VerticalScrollBarVisibility = ScrollBarVisibility.Disabled, HorizontalScrollMode = ScrollMode.Auto });
    }

    /// <summary>One layout button: name + hotkey badge, one-line "scene · rows" summary. Click = cue
    /// (graphics.layout.cue) and select, so the header offers Cue and Take for it.</summary>
    private Button LayoutButton(StudioViewModel studio, ShotLayout layout, IReadOnlyList<GraphicsPlaylistRow> rows, bool selected, bool onPreview, bool busy)
    {
        var body = new StackPanel { Spacing = 2, MinWidth = 150, MaxWidth = 240 };
        var title = new Grid { ColumnSpacing = 8 };
        title.ColumnDefinitions.Add(new() { Width = new GridLength(1, GridUnitType.Star) }); title.ColumnDefinitions.Add(new() { Width = GridLength.Auto });
        var name = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 6 };
        name.Children.Add(new TextBlock { Text = layout.Name, FontSize = 13, FontWeight = Microsoft.UI.Text.FontWeights.SemiBold, Foreground = StudioBrushes.Text, TextTrimming = TextTrimming.CharacterEllipsis });
        if (onPreview) name.Children.Add(StudioBrushes.Badge("on preview"));
        title.Children.Add(name);
        if (layout.Hotkey.Length > 0) { var badge = StudioBrushes.Badge(layout.Hotkey); Grid.SetColumn(badge, 1); title.Children.Add(badge); }
        body.Children.Add(title);
        body.Children.Add(new TextBlock { Text = LayoutSummary(studio, layout, rows), FontSize = 10, Foreground = StudioBrushes.Muted, TextTrimming = TextTrimming.CharacterEllipsis });
        var button = new Button { Content = body, Padding = new Thickness(10, 7, 10, 7), HorizontalContentAlignment = HorizontalAlignment.Stretch, Background = StudioBrushes.Panel,
            BorderBrush = selected ? StudioBrushes.Accent : StudioBrushes.Border, BorderThickness = new Thickness(1), CornerRadius = new CornerRadius(8), IsEnabled = !busy };
        ToolTipService.SetToolTip(button, $"Cue {layout.Name}: {LayoutSummary(studio, layout, rows)}. The next Take carries the picture and the graphics together.");
        button.Click += async (_, _) => { _selectedLayoutId = layout.Id; await Invoke(GraphicsPlayoutActions.LayoutCue, layout.Id); };
        return button;
    }

    /// <summary>Edit layouts flyout: choose a layout (or New), name it, pick the scene, tick the
    /// rows, set the Shift+F key and whether it hides other graphics. Save goes through
    /// graphics.layout.set, Remove through graphics.layout.remove.</summary>
    private void OpenLayoutEditor(FrameworkElement anchor, StudioViewModel studio, IReadOnlyList<GraphicsPlaylistRow> rows)
    {
        var flyout = new Flyout { Placement = Microsoft.UI.Xaml.Controls.Primitives.FlyoutPlacementMode.Bottom };
        var panel = new StackPanel { Spacing = 10, MinWidth = 320, MaxWidth = 380 };
        flyout.Content = new ScrollViewer { Content = panel, VerticalScrollBarVisibility = ScrollBarVisibility.Auto, MaxHeight = 560 };
        var layouts = studio.GraphicsPlayout.Layouts;
        var picker = new ComboBox { Header = "Layout", ItemsSource = layouts.Select(l => l.Name).Append("New layout").ToArray(), HorizontalAlignment = HorizontalAlignment.Stretch,
            SelectedIndex = Math.Max(0, layouts.ToList().FindIndex(l => l.Id == _selectedLayoutId)) };
        if (layouts.Count == 0) picker.SelectedIndex = 0;
        var form = new StackPanel { Spacing = 10 };
        void Build()
        {
            form.Children.Clear();
            var existing = picker.SelectedIndex >= 0 && picker.SelectedIndex < layouts.Count ? layouts[picker.SelectedIndex] : null;
            var scenes = studio.Scenes.ToArray();
            var name = new TextBox { Header = "Name", Text = existing?.Name ?? "", MaxLength = 128, PlaceholderText = "Open, Two-up, Panel..." };
            var scene = new ComboBox { Header = "Scene to Preview", ItemsSource = scenes.Select(s => s.Name).Prepend("No scene change").ToArray(), HorizontalAlignment = HorizontalAlignment.Stretch,
                SelectedIndex = existing is null ? 0 : Math.Max(0, Array.FindIndex(scenes, s => s.Id == existing.SceneId) + 1) };
            if (existing is { SceneId.Length: > 0 } && scenes.All(s => s.Id != existing.SceneId))
                form.Children.Add(new TextBlock { Text = $"Scene '{existing.SceneId}' is no longer in this show; this layout is refused until you pick another.", FontSize = 11, Foreground = StudioBrushes.Program, TextWrapping = TextWrapping.Wrap });
            form.Children.Add(name); form.Children.Add(scene);
            form.Children.Add(new TextBlock { Text = "Graphics that ride the Take", FontSize = 12, Foreground = StudioBrushes.Text });
            var ticks = rows.Select(r => new CheckBox { Content = r.Name, Tag = r.Id, IsChecked = existing?.RowIds.Contains(r.Id) == true }).ToArray();
            foreach (var tick in ticks) form.Children.Add(tick);
            var hotkey = new ComboBox { Header = "Hotkey", ItemsSource = LayoutHotkeyChoices, SelectedIndex = Math.Max(0, Array.IndexOf(LayoutHotkeyChoices, existing?.Hotkey ?? "")), HorizontalAlignment = HorizontalAlignment.Stretch };
            form.Children.Add(hotkey);
            var others = new ToggleSwitch { Header = "Other graphics", IsOn = existing?.PlayOutOthers == true, OnContent = "Play out Program graphics not in this layout", OffContent = "Leave other Program graphics alone" };
            form.Children.Add(others);
            var buttons = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 8 };
            var save = StudioBrushes.AccentButton(existing is null ? "Add layout" : "Save layout");
            save.Click += async (_, _) => {
                var trimmed = name.Text.Trim();
                if (trimmed.Length == 0) { _status.Text = "Give the layout a name."; return; }
                var id = existing?.Id ?? Slug(trimmed, layouts.Select(l => l.Id));
                var layout = new ShotLayout { Id = id, Name = trimmed, SceneId = scene.SelectedIndex > 0 ? scenes[scene.SelectedIndex - 1].Id : "",
                    RowIds = ticks.Where(t => t.IsChecked == true).Select(t => (string)t.Tag).ToArray(),
                    Hotkey = hotkey.SelectedIndex > 0 ? LayoutHotkeyChoices[hotkey.SelectedIndex] : "", PlayOutOthers = others.IsOn };
                _selectedLayoutId = id;
                await Invoke(GraphicsPlayoutActions.LayoutSet, JsonSerializer.Serialize(layout, LayoutJson));
                flyout.Hide();
            };
            buttons.Children.Add(save);
            if (existing is not null) {
                var remove = StudioBrushes.GhostButton("Clear cue");
                remove.Click += async (_, _) => { if (_selectedLayoutId == existing.Id) _selectedLayoutId = ""; await Invoke(GraphicsPlayoutActions.LayoutRemove, existing.Id); flyout.Hide(); };
                buttons.Children.Add(remove);
            }
            form.Children.Add(buttons);
            form.Children.Add(new TextBlock { Text = "Shift+F1..F11 cue a layout; Shift+F12 plays everything out. Cue selects the scene to Preview and stages the graphics; Take carries both.", FontSize = 10, Foreground = StudioBrushes.Muted, TextWrapping = TextWrapping.Wrap });
        }
        picker.SelectionChanged += (_, _) => Build();
        panel.Children.Add(new TextBlock { Text = "EDIT LAYOUTS", FontSize = 10, CharacterSpacing = 160, FontFamily = StudioBrushes.Mono, Foreground = StudioBrushes.Muted });
        panel.Children.Add(picker); panel.Children.Add(form);
        Build();
        flyout.ShowAt(anchor);
    }

    private static string Slug(string name, IEnumerable<string> taken)
    {
        var chars = name.ToLowerInvariant().Select(c => c is >= 'a' and <= 'z' or >= '0' and <= '9' ? c : '-').ToArray();
        var slug = string.Join('-', new string(chars).Split('-', StringSplitOptions.RemoveEmptyEntries));
        if (slug.Length == 0 || slug[0] is < 'a' or > 'z') slug = "layout" + (slug.Length == 0 ? "" : "-" + slug);
        if (slug.Length > 40) slug = slug[..40].TrimEnd('-');
        var set = taken.ToHashSet(StringComparer.Ordinal); var candidate = slug; var n = 2;
        while (set.Contains(candidate)) candidate = slug + "-" + n++;
        return candidate;
    }

    private FrameworkElement RowCard(GraphicsPlaylistRow row, IReadOnlyList<GraphicsPlaylistEntry> guests, string subject, bool busy)
    {
        var selected = _selectedKey == row.Id || (row.Kind == GraphicsPlaylistRow.Guest && _selectedKey.StartsWith(row.Id + "|", StringComparison.Ordinal));
        var card = new Border { Background = StudioBrushes.Panel, BorderBrush = selected ? StudioBrushes.Accent : StudioBrushes.Border, BorderThickness = new Thickness(1), CornerRadius = new CornerRadius(8), Padding = new Thickness(10, 8, 10, 8) };
        var body = new StackPanel { Spacing = 6 }; card.Child = body;
        var head = new Grid { ColumnSpacing = 10 };
        foreach (var width in new[] { GridLength.Auto, new GridLength(1, GridUnitType.Star), GridLength.Auto, GridLength.Auto }) head.ColumnDefinitions.Add(new() { Width = width });
        head.Children.Add(StudioBrushes.HotkeyBadge(row.Hotkey));
        var text = new StackPanel { Spacing = 1, VerticalAlignment = VerticalAlignment.Center };
        text.Children.Add(new TextBlock { Text = row.Name, FontSize = 13, FontWeight = Microsoft.UI.Text.FontWeights.SemiBold, Foreground = StudioBrushes.Text, TextTrimming = TextTrimming.CharacterEllipsis });
        text.Children.Add(new TextBlock { Text = Caption(row) + (row.PlayOutAfterMs > 0 ? $" · out after {row.PlayOutAfterMs / 1000} s" : ""), FontSize = 10, Foreground = StudioBrushes.Muted, TextTrimming = TextTrimming.CharacterEllipsis });
        Grid.SetColumn(text, 1); head.Children.Add(text);
        if (row.Kind != GraphicsPlaylistRow.Guest) {
            var entry = GraphicsPlayoutService.EntryFor(row); var state = StateOf(GraphicsPlayoutService.InstanceIdFor(entry));
            var dot = new Ellipse { Width = 9, Height = 9, Fill = DotBrush(state), VerticalAlignment = VerticalAlignment.Center }; Grid.SetColumn(dot, 2); head.Children.Add(dot);
            var actions = ActionButtons(row.Id, "", state, busy, compact: Compact); Grid.SetColumn(actions, 3); head.Children.Add(actions);
        } else {
            var count = new TextBlock { Text = guests.Count == 0 ? "no sources" : guests.Count == 1 ? "1 guest" : guests.Count + " guests", FontSize = 10, Foreground = StudioBrushes.Muted, VerticalAlignment = VerticalAlignment.Center };
            Grid.SetColumn(count, 2); head.Children.Add(count);
        }
        body.Children.Add(head);
        if (row.Kind == GraphicsPlaylistRow.Guest) {
            if (guests.Count == 0) body.Children.Add(new TextBlock { Text = "No sources in the show yet. Assign a source on the Sources page and it appears here.", FontSize = 11, Foreground = StudioBrushes.Muted, TextWrapping = TextWrapping.Wrap, Margin = new Thickness(46, 0, 0, 0) });
            foreach (var guest in guests) body.Children.Add(GuestEntry(row, guest, guest.SourceId == subject, busy));
        }
        card.Tapped += (_, _) => Select(row.Id);
        return card;
    }

    private FrameworkElement GuestEntry(GraphicsPlaylistRow row, GraphicsPlaylistEntry entry, bool isSubject, bool busy)
    {
        var key = row.Id + "|" + entry.SourceId; var state = StateOf(GraphicsPlayoutService.InstanceIdFor(entry));
        var line = new Grid { ColumnSpacing = 10, Margin = new Thickness(46, 0, 0, 0), Padding = new Thickness(8, 5, 8, 5), CornerRadius = new CornerRadius(6),
            Background = _selectedKey == key ? StudioBrushes.SurfaceRaised : new SolidColorBrush(Microsoft.UI.Colors.Transparent) };
        foreach (var width in new[] { GridLength.Auto, new GridLength(1, GridUnitType.Star), GridLength.Auto }) line.ColumnDefinitions.Add(new() { Width = width });
        line.Children.Add(new Ellipse { Width = 9, Height = 9, Fill = DotBrush(state), VerticalAlignment = VerticalAlignment.Center });
        var text = new StackPanel { Spacing = 0, VerticalAlignment = VerticalAlignment.Center };
        var name = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 6 };
        name.Children.Add(new TextBlock { Text = entry.Label.Length > 0 ? entry.Label : entry.SourceId, FontSize = 12, FontWeight = Microsoft.UI.Text.FontWeights.SemiBold, Foreground = StudioBrushes.Text, TextTrimming = TextTrimming.CharacterEllipsis });
        if (isSubject) name.Children.Add(StudioBrushes.Badge("on program"));
        text.Children.Add(name);
        if (entry.Title.Length > 0) text.Children.Add(new TextBlock { Text = entry.Title, FontSize = 10, Foreground = StudioBrushes.Muted, TextTrimming = TextTrimming.CharacterEllipsis });
        Grid.SetColumn(text, 1); line.Children.Add(text);
        var actions = ActionButtons(row.Id, entry.SourceId, state, busy, compact: Compact); Grid.SetColumn(actions, 2); line.Children.Add(actions);
        line.Tapped += (_, e) => { Select(key); e.Handled = true; };
        ToolTipService.SetToolTip(line, entry.SourceId);
        return line;
    }

    /// <summary>Full: Cue + Show, or Hide when on Program, or Remove when on Preview.
    /// Compact: Show / Hide only.</summary>
    private StackPanel ActionButtons(string rowId, string sourceId, AirState state, bool busy, bool compact)
    {
        var panel = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 6, VerticalAlignment = VerticalAlignment.Center };
        var instanceId = GraphicsPlayoutService.InstanceIdFor(rowId, sourceId);
        object?[] target = sourceId.Length == 0 ? [rowId] : [rowId, sourceId];
        Button Add(Button button, string actionId, params object?[] args) { button.IsEnabled = !busy; button.Click += async (_, e) => await Invoke(actionId, args); panel.Children.Add(button); return button; }
        if (compact) {
            if (state == AirState.Program) Add(StudioBrushes.ChipButton("Hide"), GraphicsPlayoutActions.PlayOut, instanceId);
            else Add(StudioBrushes.ChipButton("Show"), GraphicsPlayoutActions.PlayIn, target);
            return panel;
        }
        switch (state) {
            case AirState.Program: Add(StudioBrushes.ChipButton("Hide"), GraphicsPlayoutActions.PlayOut, instanceId); break;
            case AirState.Preview:
                Add(StudioBrushes.ChipButton("Show"), GraphicsPlayoutActions.PlayIn, target);
                Add(StudioBrushes.ChipButton("Clear cue"), GraphicsPlayoutActions.PreviewRemove, instanceId); break;
            default:
                Add(StudioBrushes.ChipButton("Cue"), GraphicsPlayoutActions.ToPreview, target);
                var playIn = Add(StudioBrushes.ChipButton("Show"), GraphicsPlayoutActions.PlayIn, target);
                playIn.Background = StudioBrushes.Accent; playIn.Foreground = new SolidColorBrush(Windows.UI.Color.FromArgb(255, 0x06, 0x17, 0x0D)); playIn.BorderThickness = new Thickness(0);
                break;
        }
        return panel;
    }

    private void Select(string key) { if (_selectedKey == key) return; _selectedKey = key; _listSignature = ""; Refresh(); }

    // ---- empty state ---------------------------------------------------------------------
    private FrameworkElement EmptyState(bool busy)
    {
        var host = new Grid { MinHeight = Compact ? 200 : 260 };
        host.Children.Add(new Rectangle { Stroke = StudioBrushes.Border, StrokeThickness = 1.5, StrokeDashArray = [4, 3], RadiusX = 10, RadiusY = 10, Fill = StudioBrushes.Panel });
        var body = new StackPanel { Spacing = 12, Padding = new Thickness(Compact ? 16 : 28), MaxWidth = 720, HorizontalAlignment = HorizontalAlignment.Center, VerticalAlignment = VerticalAlignment.Center };
        body.Children.Add(new TextBlock { Text = "Your graphics will live here", FontSize = Compact ? 15 : 18, FontWeight = Microsoft.UI.Text.FontWeights.SemiBold, Foreground = StudioBrushes.Text, HorizontalAlignment = HorizontalAlignment.Center, TextAlignment = TextAlignment.Center });
        body.Children.Add(new TextBlock { Text = "Lower thirds, a title, your logo, a LIVE bug and a sponsor card, each on its own F-key. Nothing goes on air until you say so.", FontSize = 12, Foreground = StudioBrushes.Muted, TextWrapping = TextWrapping.Wrap, TextAlignment = TextAlignment.Center });
        var add = StudioBrushes.AccentButton("＋ Add your first graphic"); add.HorizontalAlignment = HorizontalAlignment.Center; add.IsEnabled = !busy;
        add.Click += async (_, _) => await Invoke(GraphicsPlayoutActions.PlaylistDefault, GraphicsShowStarters.All[0].Id); body.Children.Add(add);
        var verbs = new Grid { ColumnSpacing = 12, Margin = new Thickness(0, 6, 0, 0) };
        var steps = new[] { ("1", "Add", "Pick a graphic; it joins the playlist with a hotkey."), ("2", "Show / Cue", "Show puts it on air now; Cue stages it to ride the next Take."), ("3", "Hide", "Hide takes it off, or let it hide itself after 8 or 15 seconds.") };
        if (Compact) { verbs.RowSpacing = 8; for (var i = 0; i < steps.Length; i++) verbs.RowDefinitions.Add(new() { Height = GridLength.Auto }); }
        else for (var i = 0; i < steps.Length; i++) verbs.ColumnDefinitions.Add(new() { Width = new GridLength(1, GridUnitType.Star) });
        for (var i = 0; i < steps.Length; i++) {
            var step = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 8 };
            step.Children.Add(new Border { Width = 22, Height = 22, CornerRadius = new CornerRadius(11), Background = StudioBrushes.SurfaceRaised, BorderBrush = StudioBrushes.Accent, BorderThickness = new Thickness(1),
                Child = new TextBlock { Text = steps[i].Item1, FontSize = 11, FontFamily = new FontFamily("Consolas"), Foreground = StudioBrushes.Accent, HorizontalAlignment = HorizontalAlignment.Center, VerticalAlignment = VerticalAlignment.Center } });
            var copy = new StackPanel { Spacing = 1 };
            copy.Children.Add(new TextBlock { Text = steps[i].Item2, FontSize = 12, FontWeight = Microsoft.UI.Text.FontWeights.SemiBold, Foreground = StudioBrushes.Text });
            copy.Children.Add(new TextBlock { Text = steps[i].Item3, FontSize = 10, Foreground = StudioBrushes.Muted, TextWrapping = TextWrapping.Wrap });
            step.Children.Add(copy);
            if (Compact) Grid.SetRow(step, i); else Grid.SetColumn(step, i);
            verbs.Children.Add(step);
        }
        body.Children.Add(verbs);
        var links = new StackPanel { Orientation = Compact ? Orientation.Vertical : Orientation.Horizontal, Spacing = Compact ? 0 : 14, HorizontalAlignment = HorizontalAlignment.Center };
        foreach (var (label, anchor) in new[] { ("Add a graphic", "add-a-graphic"), ("Show vs Cue", "play-in-vs-to-preview"), ("Hotkeys F1–F12", "hotkeys"), ("Guest names from Sources", "guest-names-from-sources") })
            links.Children.Add(new HyperlinkButton { Content = label, NavigateUri = new Uri(DocsBase + "#" + anchor), FontSize = 11, Padding = new Thickness(4, 2, 4, 2), Foreground = StudioBrushes.Accent });
        body.Children.Add(links);
        host.Children.Add(body);
        return host;
    }

    // ---- inspector -----------------------------------------------------------------------
    private void RefreshInspector(IReadOnlyList<GraphicsPlaylistRow> rows, IReadOnlyList<GraphicsPlaylistEntry> entries)
    {
        var rowId = _selectedKey.Split('|')[0]; var sourceId = _selectedKey.Contains('|') ? _selectedKey[(rowId.Length + 1)..] : "";
        var row = rows.FirstOrDefault(r => r.Id == rowId);
        var entry = row is null ? null : sourceId.Length == 0 ? null : entries.FirstOrDefault(e => e.RowId == rowId && e.SourceId == sourceId);
        var signature = row is null ? "" : $"{row.Id}:{row.Name}:{row.Hotkey}:{row.PlayOutAfterMs}:{row.FollowSources}:{TitleFieldKey(row)}:{(TitleFieldKey(row) is { } k ? FieldText(row, k) : "")}:{entry?.SourceId}:{entry?.Label}:{entry?.Title}";
        if (signature == _inspectorSignature) return;
        _inspectorSignature = signature; _inspector.Children.Clear(); _loading = true;
        try {
            _inspector.Children.Add(new TextBlock { Text = "INSPECTOR", FontSize = 10, CharacterSpacing = 160, FontFamily = StudioBrushes.Mono, Foreground = StudioBrushes.Muted });
            if (row is null) { _inspector.Children.Add(new TextBlock { Text = "Select a row or a guest to edit its name, title line and timing.", FontSize = 12, Foreground = StudioBrushes.Muted, TextWrapping = TextWrapping.Wrap }); return; }
            var id = row.Id;
            _inspector.Children.Add(new TextBlock { Text = entry is null ? row.Name : entry.Label, FontSize = 16, FontWeight = Microsoft.UI.Text.FontWeights.SemiBold, Foreground = StudioBrushes.Text, TextWrapping = TextWrapping.Wrap });
            _inspector.Children.Add(new TextBlock { Text = entry is null ? Caption(row) : "Guest · " + entry.SourceId, FontSize = 11, Foreground = StudioBrushes.Muted, TextWrapping = TextWrapping.Wrap });

            var name = new TextBox { Header = "Name", Text = row.Name, MaxLength = 128 };
            ToolTipService.SetToolTip(name, "The playlist row name shown on chips and in the list.");
            void CommitName() { if (!_loading && name.Text.Trim() is { Length: > 0 } value && value != row.Name) _ = SaveRowAsync(id, r => r with { Name = value }); }
            name.LostFocus += (_, _) => CommitName(); name.KeyDown += (_, e) => { if (e.Key == Windows.System.VirtualKey.Enter) CommitName(); };
            _inspector.Children.Add(name);

            if (row.Kind == GraphicsPlaylistRow.Guest && row.FollowSources) {
                _inspector.Children.Add(new TextBox { Header = "Title line", Text = entry?.Title ?? "", IsReadOnly = true, PlaceholderText = "From the Sources page" });
                var edit = StudioBrushes.GhostButton("Edit names in Sources →"); edit.HorizontalAlignment = HorizontalAlignment.Stretch;
                edit.Click += (_, _) => { if (_attached is { } s) s.ActiveTab = StudioTab.Sources; }; _inspector.Children.Add(edit);
            } else if (TitleFieldKey(row) is { } key) {
                var title = new TextBox { Header = "Title line", Text = FieldText(row, key), MaxLength = 512 };
                void CommitTitle() { if (!_loading && title.Text != FieldText(row, key)) _ = SaveRowAsync(id, r => WithField(r, key, title.Text)); }
                title.LostFocus += (_, _) => CommitTitle(); title.KeyDown += (_, e) => { if (e.Key == Windows.System.VirtualKey.Enter) CommitTitle(); };
                _inspector.Children.Add(title);
            }

            if (row.Kind == GraphicsPlaylistRow.Guest) {
                var follow = new ToggleSwitch { Header = "From Sources", IsOn = row.FollowSources, OnContent = "Names and titles follow the Sources page", OffContent = "Uses this row's own text" };
                follow.Toggled += (_, _) => { if (!_loading && follow.IsOn != row.FollowSources) _ = SaveRowAsync(id, r => r with { FollowSources = follow.IsOn }); };
                _inspector.Children.Add(follow);
            }

            var playOut = new ComboBox { Header = "Hides after", ItemsSource = PlayOutChoices.Select(c => c.Label).ToArray(), SelectedIndex = Math.Max(0, Array.FindIndex(PlayOutChoices, c => c.Ms == row.PlayOutAfterMs)), HorizontalAlignment = HorizontalAlignment.Stretch };
            playOut.SelectionChanged += (_, _) => { if (!_loading && playOut.SelectedIndex >= 0 && PlayOutChoices[playOut.SelectedIndex].Ms != row.PlayOutAfterMs) _ = SaveRowAsync(id, r => r with { PlayOutAfterMs = PlayOutChoices[playOut.SelectedIndex].Ms }); };
            _inspector.Children.Add(playOut);

            var hotkey = new ComboBox { Header = "Hotkey", ItemsSource = HotkeyChoices, SelectedIndex = Math.Max(0, Array.IndexOf(HotkeyChoices, row.Hotkey)), HorizontalAlignment = HorizontalAlignment.Stretch };
            hotkey.SelectionChanged += async (_, _) => { if (_loading || hotkey.SelectedIndex < 0) return; var next = hotkey.SelectedIndex == 0 ? "" : HotkeyChoices[hotkey.SelectedIndex]; if (next != row.Hotkey) await Invoke(GraphicsPlayoutActions.HotkeySet, id, next); };
            _inspector.Children.Add(hotkey);
            _inspector.Children.Add(new TextBlock { Text = "Hotkeys work on the Studio and Graphics tabs when no text box has focus. Shift+F12 plays everything out.", FontSize = 10, Foreground = StudioBrushes.Muted, TextWrapping = TextWrapping.Wrap });

            var design = StudioBrushes.GhostButton("Open in Design"); design.HorizontalAlignment = HorizontalAlignment.Stretch;
            ToolTipService.SetToolTip(design, "Switch to Design to change the look. Program and Preview are unchanged.");
            design.Click += (_, _) => _attached?.OpenGraphicInDesign(id);
            _inspector.Children.Add(design);
            if (row.TemplateId.Length == 0 && row.Image is null)
                _inspector.Children.Add(new TextBlock { Text = "This row has no graphic yet. Open it in Design and import a full-canvas image to give it one.", FontSize = 11, Foreground = StudioBrushes.Program, TextWrapping = TextWrapping.Wrap });
        } finally { _loading = false; }
    }
}
