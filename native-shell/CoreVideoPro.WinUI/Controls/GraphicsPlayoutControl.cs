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
/// the Guest row expanded into its roster entries, with Play In / To Preview / Play Out /
/// Remove per item. Full form adds the inspector; <see cref="Compact"/> (Studio right panel)
/// shows rows and entries with Play In / Play Out only. Row state comes from the native strip,
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
    private static readonly (string Label, int Ms)[] PlayOutChoices = [("Manual", 0), ("8 s", 8000), ("15 s", 15000)];

    private StudioViewModel? _attached;
    private GraphicsWorkspaceLibrary? _library;
    private readonly Grid _root = new() { ColumnSpacing = 12 };
    private readonly ColumnDefinition _inspectorColumn = new() { Width = new GridLength(300) };
    private readonly StackPanel _list = new() { Spacing = 8 };
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
        left.RowDefinitions.Add(new() { Height = GridLength.Auto }); left.RowDefinitions.Add(new() { Height = new GridLength(1, GridUnitType.Star) }); left.RowDefinitions.Add(new() { Height = GridLength.Auto });
        var header = new Grid { ColumnSpacing = 8 };
        header.ColumnDefinitions.Add(new() { Width = new GridLength(1, GridUnitType.Star) }); header.ColumnDefinitions.Add(new() { Width = GridLength.Auto });
        header.Children.Add(new TextBlock { Text = "PLAYLIST", FontSize = 10, CharacterSpacing = 160, FontFamily = StudioBrushes.Mono, Foreground = StudioBrushes.Muted, VerticalAlignment = VerticalAlignment.Center });
        _add = StudioBrushes.GhostButton("＋ Add graphic"); _add.Click += async (_, _) => await AddGraphicAsync(); Grid.SetColumn(_add, 1); header.Children.Add(_add);
        ToolTipService.SetToolTip(_add, "Add a graphic. An empty playlist installs the default five rows; otherwise this opens Design.");
        left.Children.Add(header);
        var scroll = new ScrollViewer { Content = _list, VerticalScrollBarVisibility = ScrollBarVisibility.Auto, HorizontalScrollBarVisibility = ScrollBarVisibility.Disabled };
        Grid.SetRow(scroll, 1); left.Children.Add(scroll);
        _status.Foreground = StudioBrushes.Muted; Grid.SetRow(_status, 2); left.Children.Add(_status);
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
        _listSignature = ""; Refresh();
    }

    // ---- lifecycle -----------------------------------------------------------------------
    private void Attach()
    {
        if (Studio is null || !IsLoaded || ReferenceEquals(_attached, Studio)) return;
        Detach(); _attached = Studio; _attached.PropertyChanged += OnStudioProperty;
        _library = Studio.GraphicsLibrary; _library.Changed += OnLibraryChanged; _ = _library.LoadAsync();
        _listSignature = ""; _inspectorSignature = ""; Refresh();
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
            or nameof(StudioViewModel.GraphicsPlayoutBusy) or nameof(StudioViewModel.GraphicsProgramSubjectSourceId)) Refresh();
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
        if (!Compact) RefreshInspector(rows, entries);
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

    /// <summary>Full: To Preview + Play In, or Play Out when on Program, or Remove when on Preview.
    /// Compact: Play In / Play Out only.</summary>
    private StackPanel ActionButtons(string rowId, string sourceId, AirState state, bool busy, bool compact)
    {
        var panel = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 6, VerticalAlignment = VerticalAlignment.Center };
        var instanceId = GraphicsPlayoutService.InstanceIdFor(rowId, sourceId);
        object?[] target = sourceId.Length == 0 ? [rowId] : [rowId, sourceId];
        Button Add(Button button, string actionId, params object?[] args) { button.IsEnabled = !busy; button.Click += async (_, e) => await Invoke(actionId, args); panel.Children.Add(button); return button; }
        if (compact) {
            if (state == AirState.Program) Add(StudioBrushes.ChipButton("Play Out"), GraphicsPlayoutActions.PlayOut, instanceId);
            else Add(StudioBrushes.ChipButton("Play In"), GraphicsPlayoutActions.PlayIn, target);
            return panel;
        }
        switch (state) {
            case AirState.Program: Add(StudioBrushes.ChipButton("Play Out"), GraphicsPlayoutActions.PlayOut, instanceId); break;
            case AirState.Preview:
                Add(StudioBrushes.ChipButton("Play In"), GraphicsPlayoutActions.PlayIn, target);
                Add(StudioBrushes.ChipButton("Remove"), GraphicsPlayoutActions.PreviewRemove, instanceId); break;
            default:
                Add(StudioBrushes.ChipButton("To Preview"), GraphicsPlayoutActions.ToPreview, target);
                var playIn = Add(StudioBrushes.ChipButton("Play In"), GraphicsPlayoutActions.PlayIn, target);
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
        var steps = new[] { ("1", "Add", "Pick a graphic; it joins the playlist with a hotkey."), ("2", "Play In / To Preview", "Play In goes to Program now; To Preview stages it to ride the next Take."), ("3", "Play Out", "Hide it, or let it play out after 8 or 15 seconds.") };
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
        foreach (var (label, anchor) in new[] { ("Add a graphic", "add-a-graphic"), ("Play In vs To Preview", "play-in-vs-to-preview"), ("Hotkeys F1–F12", "hotkeys"), ("Guest names from Sources", "guest-names-from-sources") })
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

            var playOut = new ComboBox { Header = "Plays out after", ItemsSource = PlayOutChoices.Select(c => c.Label).ToArray(), SelectedIndex = Math.Max(0, Array.FindIndex(PlayOutChoices, c => c.Ms == row.PlayOutAfterMs)), HorizontalAlignment = HorizontalAlignment.Stretch };
            playOut.SelectionChanged += (_, _) => { if (!_loading && playOut.SelectedIndex >= 0 && PlayOutChoices[playOut.SelectedIndex].Ms != row.PlayOutAfterMs) _ = SaveRowAsync(id, r => r with { PlayOutAfterMs = PlayOutChoices[playOut.SelectedIndex].Ms }); };
            _inspector.Children.Add(playOut);

            var hotkey = new ComboBox { Header = "Hotkey", ItemsSource = HotkeyChoices, SelectedIndex = Math.Max(0, Array.IndexOf(HotkeyChoices, row.Hotkey)), HorizontalAlignment = HorizontalAlignment.Stretch };
            hotkey.SelectionChanged += async (_, _) => { if (_loading || hotkey.SelectedIndex < 0) return; var next = hotkey.SelectedIndex == 0 ? "" : HotkeyChoices[hotkey.SelectedIndex]; if (next != row.Hotkey) await Invoke(GraphicsPlayoutActions.HotkeySet, id, next); };
            _inspector.Children.Add(hotkey);
            _inspector.Children.Add(new TextBlock { Text = "Hotkeys work on the Studio and Graphics tabs when no text box has focus. Shift+F12 plays everything out.", FontSize = 10, Foreground = StudioBrushes.Muted, TextWrapping = TextWrapping.Wrap });

            var design = StudioBrushes.GhostButton("Open in Design"); design.HorizontalAlignment = HorizontalAlignment.Stretch;
            ToolTipService.SetToolTip(design, "Switch to Design to change the look. Program and Preview are unchanged.");
            design.Click += (_, _) => { if (_attached is { } s) { s.GraphicsMode = StudioViewModel.GraphicsModeDesign; s.ActiveTab = StudioTab.Overlays; } };
            _inspector.Children.Add(design);
            if (row.TemplateId.Length == 0 && row.Image is null)
                _inspector.Children.Add(new TextBlock { Text = "This row has no graphic yet. Open it in Design and import a full-canvas image to give it one.", FontSize = 11, Foreground = StudioBrushes.Program, TextWrapping = TextWrapping.Wrap });
        } finally { _loading = false; }
    }
}
