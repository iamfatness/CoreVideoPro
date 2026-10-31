using System.Text.Json;
using CoreVideoPro.MediaCore.Models;
using CoreVideoPro.MediaCore.Services;
using CoreVideoPro.WinUI.Services;
using CoreVideoPro.WinUI.ViewModels;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Controls.Primitives;
using Microsoft.UI.Xaml.Media;

namespace CoreVideoPro.WinUI.Controls;

/// <summary>Design mode's default surface (#1076 slice 3, titler spec "basic designer"): make a
/// graphic in four steps without the looks library, private cues or layer editor. 1 pick what
/// you are making, 2 pick a look, 3 fill it in while the native preview updates, 4 save it to the
/// playlist with an F-key. Everything here edits the workspace session's PRIVATE draft; nothing
/// reaches Program or Preview until the operator shows the row from Operate.</summary>
public sealed class GraphicsBuilderControl : UserControl
{
    public static readonly DependencyProperty StudioProperty = DependencyProperty.Register(nameof(Studio), typeof(StudioViewModel),
        typeof(GraphicsBuilderControl), new PropertyMetadata(null, (d, _) => ((GraphicsBuilderControl)d).Attach()));
    public StudioViewModel? Studio { get => (StudioViewModel?)GetValue(StudioProperty); set => SetValue(StudioProperty, value); }

    private static readonly string[] HotkeyChoices = ["None", .. Enumerable.Range(1, 12).Select(n => "F" + n)];
    private static readonly (string Label, int Ms)[] PlayOutChoices = [("Stays until Hide", 0), ("8 seconds", 8000), ("15 seconds", 15000)];

    private readonly Grid _root = new() { ColumnSpacing = 16, RowSpacing = 12 };
    private readonly StackPanel _header = new() { Orientation = Orientation.Horizontal, Spacing = 10 };
    private readonly StackPanel _yours = new() { Orientation = Orientation.Horizontal, Spacing = 6 };
    private readonly StackPanel _shelf = new() { Spacing = 14, MinWidth = 260 };
    private readonly StackPanel _kinds = new() { Spacing = 6 };
    private readonly StackPanel _looks = new() { Spacing = 8 };
    private readonly TextBlock _looksNote = Note("");
    private readonly StackPanel _center = new() { Spacing = 8 };
    private readonly GraphicsAuthoringCanvas _canvas = new() { WholeGraphic = true, MinHeight = 240 };
    private readonly TextBlock _previewLine = Note("Private preview. Nothing here is on air until you Show it from Operate.");
    private readonly StackPanel _form = new() { Spacing = 10, MinWidth = 280 };
    private readonly StackPanel _fields = new() { Spacing = 8 };
    private readonly StackPanel _colors = new() { Spacing = 6 };
    private readonly ComboBox _motion = new() { Header = "Motion", ItemsSource = GraphicsBuilder.MotionPresets.Select(m => m.Label).ToArray(), SelectedIndex = 0, HorizontalAlignment = HorizontalAlignment.Stretch };
    private readonly ComboBox _speed = new() { Header = "Speed", ItemsSource = GraphicsBuilder.MotionSpeeds.Select(m => m.Label).ToArray(), SelectedIndex = 1, HorizontalAlignment = HorizontalAlignment.Stretch };
    private readonly TextBox _name = new() { Header = "Name in the playlist", MaxLength = 128, PlaceholderText = "Guest name" };
    private readonly ComboBox _hotkey = new() { Header = "Hotkey", ItemsSource = HotkeyChoices, HorizontalAlignment = HorizontalAlignment.Stretch };
    private readonly ComboBox _playOut = new() { Header = "Hides after", ItemsSource = PlayOutChoices.Select(c => c.Label).ToArray(), SelectedIndex = 0, HorizontalAlignment = HorizontalAlignment.Stretch };
    private readonly ToggleSwitch _follow = new() { Header = "Names from Sources", OnContent = "Follows whoever is on Program", OffContent = "Uses the text you type here" };
    private readonly Button _save = StudioBrushes.AccentButton("Add to playlist");
    private readonly Button _discard = StudioBrushes.GhostButton("Start over");
    private readonly Button _operate = StudioBrushes.GhostButton("Open in Operate →");
    private readonly TextBlock _status = Note("");
    private readonly Button _sponsorPick = StudioBrushes.GhostButton("Choose image…");
    private readonly TextBlock _sponsorFile = Note("PNG or JPG, 1920×1080 works best.");

    private GraphicsWorkspaceSession? _session; private StudioViewModel? _attached; private GraphicsWorkspaceLibrary? _library;
    private ColorGradeEditorViewModel? _editor; private IDisposable? _previewOwner; private bool _suspended = true;
    private string _kindId = GraphicsBuilder.Kinds[0].Id, _editingRowId = "", _formSignature = "", _looksSignature = "", _thumbnailEpoch = "";
    private string _sponsorPath = ""; private bool _loading, _dirty, _saving, _narrow;
    private readonly DispatcherTimer _debounce = new() { Interval = TimeSpan.FromMilliseconds(350) };
    private Action? _pendingCommit;

    public GraphicsBuilderControl()
    {
        _root.RowDefinitions.Add(new() { Height = GridLength.Auto }); _root.RowDefinitions.Add(new() { Height = new GridLength(1, GridUnitType.Star) });
        _root.ColumnDefinitions.Add(new() { Width = new GridLength(300) }); _root.ColumnDefinitions.Add(new() { Width = new GridLength(1, GridUnitType.Star) }); _root.ColumnDefinitions.Add(new() { Width = new GridLength(340) });

        var title = new TextBlock { Text = "Build a graphic", FontSize = 18, FontWeight = Microsoft.UI.Text.FontWeights.SemiBold, Foreground = StudioBrushes.Text, VerticalAlignment = VerticalAlignment.Center };
        var advanced = StudioBrushes.GhostButton("Advanced editor"); advanced.Click += (_, _) => { if (_attached is { } s) s.GraphicsDesignAdvanced = true; };
        ToolTipService.SetToolTip(advanced, "Looks library, private cues, layer editor, templates and automation.");
        var yoursLabel = new TextBlock { Text = "YOUR GRAPHICS", FontSize = 10, CharacterSpacing = 160, FontFamily = StudioBrushes.Mono, Foreground = StudioBrushes.Muted, VerticalAlignment = VerticalAlignment.Center, Margin = new Thickness(14, 0, 0, 0) };
        var yoursScroll = new ScrollViewer { Content = _yours, HorizontalScrollBarVisibility = ScrollBarVisibility.Auto, VerticalScrollBarVisibility = ScrollBarVisibility.Disabled, VerticalAlignment = VerticalAlignment.Center };
        var headerGrid = new Grid { ColumnSpacing = 10 };
        headerGrid.ColumnDefinitions.Add(new() { Width = GridLength.Auto }); headerGrid.ColumnDefinitions.Add(new() { Width = GridLength.Auto }); headerGrid.ColumnDefinitions.Add(new() { Width = new GridLength(1, GridUnitType.Star) }); headerGrid.ColumnDefinitions.Add(new() { Width = GridLength.Auto });
        headerGrid.Children.Add(title); Grid.SetColumn(yoursLabel, 1); headerGrid.Children.Add(yoursLabel); Grid.SetColumn(yoursScroll, 2); headerGrid.Children.Add(yoursScroll); Grid.SetColumn(advanced, 3); headerGrid.Children.Add(advanced);
        Grid.SetColumnSpan(headerGrid, 3); _root.Children.Add(headerGrid);

        // 1 + 2: what and look
        _shelf.Children.Add(Step("1", "WHAT ARE YOU MAKING"));
        foreach (var kind in GraphicsBuilder.Kinds) _kinds.Children.Add(KindTile(kind));
        _shelf.Children.Add(_kinds);
        _shelf.Children.Add(Step("2", "PICK A LOOK"));
        _shelf.Children.Add(_looks); _shelf.Children.Add(_looksNote);
        var shelfScroll = new ScrollViewer { Content = _shelf, VerticalScrollBarVisibility = ScrollBarVisibility.Auto, HorizontalScrollBarVisibility = ScrollBarVisibility.Disabled };
        Grid.SetRow(shelfScroll, 1); _root.Children.Add(shelfScroll);

        // 3: preview
        _center.Children.Add(Step("3", "PREVIEW"));
        var frame = new Border { Background = StudioBrushes.Field, BorderBrush = StudioBrushes.Border, BorderThickness = new Thickness(1), CornerRadius = new CornerRadius(8), Padding = new Thickness(6), Child = _canvas };
        _center.Children.Add(frame); _center.Children.Add(_previewLine);
        Grid.SetRow(_center, 1); Grid.SetColumn(_center, 1); _root.Children.Add(_center);

        // 4 + 5: fill in and save
        _form.Children.Add(Step("4", "FILL IT IN"));
        _form.Children.Add(_fields); _form.Children.Add(_colors);
        var motionRow = new Grid { ColumnSpacing = 8, Margin = new Thickness(0, 6, 0, 0) }; motionRow.ColumnDefinitions.Add(new() { Width = new GridLength(3, GridUnitType.Star) }); motionRow.ColumnDefinitions.Add(new() { Width = new GridLength(2, GridUnitType.Star) });
        motionRow.Children.Add(_motion); Grid.SetColumn(_speed, 1); motionRow.Children.Add(_speed); _form.Children.Add(motionRow);
        ToolTipService.SetToolTip(_motion, "How the graphic comes in and goes out on air. Shown on Program when you Show it; the preview here is still.");
        _form.Children.Add(Step("5", "SAVE TO PLAYLIST"));
        _form.Children.Add(_name); _form.Children.Add(_follow);
        var timing = new Grid { ColumnSpacing = 8 }; timing.ColumnDefinitions.Add(new() { Width = new GridLength(1, GridUnitType.Star) }); timing.ColumnDefinitions.Add(new() { Width = new GridLength(1, GridUnitType.Star) });
        timing.Children.Add(_hotkey); Grid.SetColumn(_playOut, 1); timing.Children.Add(_playOut); _form.Children.Add(timing);
        var actions = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 8 };
        actions.Children.Add(_save); actions.Children.Add(_discard); _form.Children.Add(actions);
        _form.Children.Add(_status); _operate.Visibility = Visibility.Collapsed; _form.Children.Add(_operate);
        var formScroll = new ScrollViewer { Content = _form, VerticalScrollBarVisibility = ScrollBarVisibility.Auto, HorizontalScrollBarVisibility = ScrollBarVisibility.Disabled };
        Grid.SetRow(formScroll, 1); Grid.SetColumn(formScroll, 2); _root.Children.Add(formScroll);

        _save.Click += async (_, _) => await SaveAsync();
        _discard.Click += (_, _) => StartNew();
        _operate.Click += (_, _) => { if (_attached is { } s) s.GraphicsMode = StudioViewModel.GraphicsModePlayout; };
        _follow.Toggled += (_, _) => { if (!_loading) { _dirty = true; RefreshSaveButton(); } };
        _motion.SelectionChanged += (_, _) => { if (!_loading) { _dirty = true; _speed.IsEnabled = SelectedMotionPreset != "cut"; RefreshSaveButton(); } };
        _speed.SelectionChanged += (_, _) => { if (!_loading) { _dirty = true; RefreshSaveButton(); } };
        _name.TextChanged += (_, _) => { if (!_loading) { _dirty = true; RefreshSaveButton(); } };
        _sponsorPick.Click += async (_, _) => await PickSponsorAsync();
        _debounce.Tick += (_, _) => { _debounce.Stop(); var commit = _pendingCommit; _pendingCommit = null; commit?.Invoke(); };

        Content = _root; Padding = new Thickness(0, 4, 0, 0);
        Loaded += (_, _) => Attach(); Unloaded += (_, _) => Detach();
        SizeChanged += (_, e) => Reflow(e.NewSize.Width);
    }

    // ---- lifecycle -------------------------------------------------------------------------
    private void Attach()
    {
        if (Studio is null || !IsLoaded || ReferenceEquals(_attached, Studio)) return;
        Detach();
        _attached = Studio; _session = Studio.GraphicsWorkspace; _library = Studio.GraphicsLibrary;
        _canvas.Session = _session;
        _session.Changed += OnSessionChanged; _session.PreviewChanged += OnPreviewChanged;
        _library.Changed += OnLibraryChanged; _attached.PropertyChanged += OnStudioProperty;
        _editor = new("preview:lower-third", "Graphics builder preview", new() { Lut = "none" }) { LiveEditing = false };
        _editor.PropertyChanged += OnEditorProperty;
        _ = _library.LoadAsync();
        if (_attached.GraphicsDesignRowId.Length > 0) LoadRow(_attached.GraphicsDesignRowId); else if (_session.Draft is null) StartNew(); else RefreshAll();
        UpdatePreviewLease();
    }
    private void Detach()
    {
        if (_session is not null) { _session.Changed -= OnSessionChanged; _session.PreviewChanged -= OnPreviewChanged; }
        if (_library is not null) _library.Changed -= OnLibraryChanged;
        if (_attached is not null) _attached.PropertyChanged -= OnStudioProperty;
        if (_editor is not null) _editor.PropertyChanged -= OnEditorProperty;
        _previewOwner?.Dispose(); _previewOwner = null; _canvas.Session = null;
        _session = null; _library = null; _attached = null; _editor = null;
    }
    /// <summary>The page holds the private preview only while this is the visible Design surface.</summary>
    public void SuspendPreview() { _suspended = true; UpdatePreviewLease(); }
    public void ResumePreview() { _suspended = false; UpdatePreviewLease(); RequestThumbnails(); }
    private void UpdatePreviewLease()
    {
        if (!_suspended && _attached?.IsOverlaysTab == true && _session?.Preview is not null && _session.PreviewValidation.Length == 0 && _editor is not null)
            _previewOwner ??= _attached.CreateAppearancePreview(_editor);
        else { _previewOwner?.Dispose(); _previewOwner = null; }
    }
    private void OnEditorProperty(object? sender, System.ComponentModel.PropertyChangedEventArgs e)
    {
        if (_editor is not null && e.PropertyName == nameof(ColorGradeEditorViewModel.NativeSurface)) _canvas.SurfaceState = _editor.NativeSurface;
    }
    private void OnStudioProperty(object? sender, System.ComponentModel.PropertyChangedEventArgs e)
    {
        if (e.PropertyName == nameof(StudioViewModel.GraphicsDesignRowId) && _attached is { GraphicsDesignRowId.Length: > 0 } s) LoadRow(s.GraphicsDesignRowId);
        else if (e.PropertyName is nameof(StudioViewModel.ActiveTab) or nameof(StudioViewModel.IsOverlaysTab)) UpdatePreviewLease();
    }
    private void OnSessionChanged(object? sender, EventArgs e) { RefreshAll(); RequestThumbnails(); UpdatePreviewLease(); }
    private void OnPreviewChanged(object? sender, EventArgs e) { RefreshForm(); UpdatePreviewLease(); }
    private void OnLibraryChanged(object? sender, EventArgs e) => RefreshYours();
    private void RequestThumbnails()
    {
        if (_suspended || _session is not { Busy: false, State.CoreEpoch: { Length: > 0 } epoch } || _thumbnailEpoch == epoch) return;
        _thumbnailEpoch = epoch; _ = _session.RefreshCatalogAsync();
    }

    // ---- flows -----------------------------------------------------------------------------
    private GraphicsBuilderKind Kind => GraphicsBuilder.Kind(_kindId) ?? GraphicsBuilder.Kinds[0];
    private IReadOnlyList<GraphicsPlaylistRow> Rows => _attached?.GraphicsPlayout.Playlist ?? [];

    private void StartNew()
    {
        _editingRowId = ""; _sponsorPath = ""; _dirty = false; _operate.Visibility = Visibility.Collapsed; _status.Text = "";
        if (_attached is { } s && s.GraphicsDesignRowId.Length > 0) s.GraphicsDesignRowId = "";
        _loading = true;
        try { _name.Text = ""; _follow.IsOn = Kind.Family == "lower-third"; _playOut.SelectedIndex = Kind.Family == "lower-third" ? 1 : 0; _hotkey.SelectedIndex = Math.Max(0, Array.IndexOf(HotkeyChoices, GraphicsBuilder.NextFreeHotkey(Rows))); SetMotion(null); }
        finally { _loading = false; }
        var templates = GraphicsBuilder.TemplatesFor(Kind, _session?.State);
        if (templates.Count > 0 && _session is { Busy: false }) _session.Browse(templates[0]);
        _formSignature = ""; RefreshAll();
    }

    private void LoadRow(string rowId)
    {
        if (_session is null || Rows.FirstOrDefault(r => r.Id == rowId) is not { } row) { if (_attached is { } s) s.GraphicsDesignRowId = ""; return; }
        var kind = GraphicsBuilder.KindOf(row, _session.State);
        _kindId = kind.Id; _editingRowId = row.Id; _sponsorPath = row.ImagePath ?? ""; _dirty = false; _operate.Visibility = Visibility.Collapsed; _status.Text = "";
        _loading = true;
        try {
            _name.Text = row.Name; _follow.IsOn = row.FollowSources;
            _playOut.SelectedIndex = Math.Max(0, Array.FindIndex(PlayOutChoices, c => c.Ms == row.PlayOutAfterMs));
            _hotkey.SelectedIndex = Math.Max(0, Array.IndexOf(HotkeyChoices, row.Hotkey));
            SetMotion(row.Motion);
        } finally { _loading = false; }
        if (kind.Id != GraphicsBuilder.SponsorKindId && _session.State?.Templates.FirstOrDefault(t => t.TemplateId == row.TemplateId && t.TemplateVersion == row.TemplateVersion) is { } template && !_session.Busy) {
            _session.Browse(template);
            if (_session.Draft is { } draft) _session.Edit(draft with { Fields = GraphicsBuilder.FieldsElement(row), Style = row.Style, Image = row.Image });
        }
        if (_attached is { } studio && studio.GraphicsDesignRowId == rowId) studio.GraphicsDesignRowId = "";
        _formSignature = ""; _looksSignature = ""; RefreshAll();
    }

    private void ChooseKind(GraphicsBuilderKind kind)
    {
        if (_kindId == kind.Id) return;
        _kindId = kind.Id; _editingRowId = ""; _dirty = false; _operate.Visibility = Visibility.Collapsed; _status.Text = "";
        _loading = true; try { _follow.IsOn = kind.Family == "lower-third"; _playOut.SelectedIndex = kind.Family == "lower-third" ? 1 : 0; } finally { _loading = false; }
        var templates = GraphicsBuilder.TemplatesFor(kind, _session?.State);
        if (templates.Count > 0 && _session is { Busy: false }) _session.Browse(templates[0]);
        _formSignature = ""; _looksSignature = ""; RefreshAll();
    }

    private void ChooseLook(NativeGraphicsTemplate template)
    {
        if (_session is not { Busy: false }) return;
        _session.Browse(template); _dirty = true; _formSignature = ""; RefreshAll();
    }

    private async Task PickSponsorAsync()
    {
        try {
            var picker = new Windows.Storage.Pickers.FileOpenPicker();
            foreach (var ext in new[] { ".png", ".jpg", ".jpeg" }) picker.FileTypeFilter.Add(ext);
            WinRT.Interop.InitializeWithWindow.Initialize(picker, App.MainWindowHandle);
            var file = await picker.PickSingleFileAsync();
            if (file is null) return;
            _sponsorPath = file.Path; _dirty = true; if (_name.Text.Trim().Length == 0) _name.Text = System.IO.Path.GetFileNameWithoutExtension(file.Path);
            RefreshLooks(); RefreshSaveButton();
        } catch (Exception ex) when (ex is not OutOfMemoryException) { _status.Text = "Could not open the image picker: " + ex.Message; }
    }

    private async Task SaveAsync()
    {
        if (_saving || _attached is not { } studio || _library is null) return;
        _saving = true; RefreshSaveButton();
        try {
            var rows = Rows; var existing = rows.FirstOrDefault(r => r.Id == _editingRowId);
            var hotkey = _hotkey.SelectedIndex <= 0 ? "" : HotkeyChoices[_hotkey.SelectedIndex];
            var playOut = PlayOutChoices[Math.Max(0, _playOut.SelectedIndex)].Ms;
            GraphicsPlaylistRow row; bool ok;
            if (Kind.Id == GraphicsBuilder.SponsorKindId) {
                if (_sponsorPath.Length == 0 && existing?.Image is null) { _status.Text = "Choose an image first."; return; }
                var name = _name.Text.Trim().Length > 0 ? _name.Text.Trim() : "Sponsor card";
                row = (existing ?? new GraphicsPlaylistRow { Id = GraphicsBuilder.NewRowId(name, rows), Kind = GraphicsPlaylistRow.Sponsor }) with { Name = name, Hotkey = hotkey, PlayOutAfterMs = playOut, Motion = SelectedMotion };
                ok = await _library.SavePlaylistAsync(GraphicsBuilder.Upsert(rows, row));
                if (ok && _sponsorPath.Length > 0 && _sponsorPath != existing?.ImagePath) ok = await _library.AssignPlaylistImageAsync(row.Id, _sponsorPath);
            } else {
                if (_session?.Draft is not { } draft || _session.Template is not { } template) { _status.Text = "Pick a look first."; return; }
                row = GraphicsBuilder.RowFrom(Kind, template, draft, _name.Text, hotkey, playOut, _follow.IsOn, rows, existing, SelectedMotion);
                ok = await _library.SavePlaylistAsync(GraphicsBuilder.Upsert(rows, row));
            }
            if (!ok) { _status.Text = _library.Status; return; }
            _editingRowId = row.Id; _dirty = false;
            _status.Text = $"Saved. \"{row.Name}\" is in the playlist{(row.Hotkey.Length > 0 ? " on " + row.Hotkey : "")}. Show it from Operate when you want it on air.";
            _operate.Visibility = Visibility.Visible;
            RefreshYours(); RefreshSaveButton();
        } finally { _saving = false; RefreshSaveButton(); }
    }

    // ---- rendering -------------------------------------------------------------------------
    private void RefreshAll() { RefreshKinds(); RefreshLooks(); RefreshForm(); RefreshYours(); RefreshSaveButton(); }

    private void RefreshKinds()
    {
        foreach (var child in _kinds.Children.OfType<Button>()) {
            var selected = child.Tag is GraphicsBuilderKind k && k.Id == _kindId;
            child.BorderBrush = selected ? StudioBrushes.Accent : StudioBrushes.Border; child.BorderThickness = new Thickness(selected ? 2 : 1);
            child.Background = selected ? StudioBrushes.SurfaceRaised : StudioBrushes.Panel;
        }
    }

    private void RefreshLooks()
    {
        var kind = Kind; var state = _session?.State;
        var templates = GraphicsBuilder.TemplatesFor(kind, state);
        var current = _session?.Draft; var selected = current is null ? "" : current.TemplateId + ":" + current.TemplateVersion;
        var signature = kind.Id + "|" + selected + "|" + _sponsorPath + "|" + string.Join(",", templates.Select(t => t.TemplateId + ":" + t.TemplateVersion + ":" + t.Thumbnail?.State));
        if (signature == _looksSignature) return; _looksSignature = signature;
        _looks.Children.Clear();
        if (kind.Id == GraphicsBuilder.SponsorKindId) {
            _looks.Children.Add(_sponsorPick);
            _sponsorFile.Text = _sponsorPath.Length > 0 ? System.IO.Path.GetFileName(_sponsorPath) : "PNG or JPG, 1920×1080 works best.";
            _looks.Children.Add(_sponsorFile); _looksNote.Text = "A sponsor card is your own full-frame image; no template needed.";
            return;
        }
        foreach (var template in templates) {
            var isSelected = template.TemplateId + ":" + template.TemplateVersion == selected;
            var card = new Button { Content = new GraphicsTemplateCard(template, compact: false), Padding = new Thickness(6), CornerRadius = new CornerRadius(8),
                Background = isSelected ? StudioBrushes.SurfaceRaised : StudioBrushes.Panel, BorderBrush = isSelected ? StudioBrushes.Accent : StudioBrushes.Border, BorderThickness = new Thickness(isSelected ? 2 : 1),
                HorizontalContentAlignment = HorizontalAlignment.Stretch, HorizontalAlignment = HorizontalAlignment.Stretch };
            card.Click += (_, _) => ChooseLook(template);
            _looks.Children.Add(card);
        }
        _looksNote.Text = templates.Count == 0 ? (state is null ? "Waiting for native graphics." : $"No {kind.Label.ToLowerInvariant()} looks are installed. Import a package in the Advanced editor.")
            : "Looks come from the installed templates. Your colors apply to any of them.";
    }

    private void RefreshForm()
    {
        var kind = Kind; var draft = _session?.Draft; var template = _session?.Template;
        var signature = kind.Id + "|" + _editingRowId + "|" + (template is null ? "" : template.TemplateId + ":" + template.TemplateVersion) + "|" + GraphicsWorkspaceSession.SupportsLogo(template) + "|" + (draft?.Image?.AssetId ?? "");
        _follow.Visibility = kind.Family == "lower-third" ? Visibility.Visible : Visibility.Collapsed;
        if (signature == _formSignature) { RefreshColorSwatches(); return; }
        _formSignature = signature; _fields.Children.Clear(); _colors.Children.Clear(); _loading = true;
        try {
            if (kind.Id == GraphicsBuilder.SponsorKindId) { _fields.Children.Add(Note("Nothing to fill in: the image is the graphic. Give it a name and a hotkey below.")); return; }
            if (draft is null || template is null) { _fields.Children.Add(Note("Pick a look to fill it in.")); return; }
            var fields = template.Fields.Count > 0 ? template.Fields : [new() { Id = "name", Type = "string", MaxLength = 256 }, new() { Id = "title", Type = "string", MaxLength = 512 }];
            foreach (var field in fields) {
                if (field.Id == "time" && template.Family is "clock" or "countdown") { _fields.Children.Add(Note("The time is live on air; set the clock mode from Operate once this is in the playlist.")); continue; }
                if (field.Type == "image" && field.Id == "logo" && GraphicsWorkspaceSession.SupportsLogo(template)) continue;
                var value = draft.Fields.ValueKind == JsonValueKind.Object && draft.Fields.TryGetProperty(field.Id, out var v) ? v : field.Default ?? JsonSerializer.SerializeToElement("");
                _fields.Children.Add(FieldControl(field, value));
            }
            if (GraphicsWorkspaceSession.SupportsLogo(template)) {
                var logoRow = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 8 };
                var pick = StudioBrushes.GhostButton(draft.Image is null ? "Choose logo…" : "Replace logo…"); pick.Click += async (_, _) => await PickLogoAsync();
                logoRow.Children.Add(pick);
                if (draft.Image is not null) { var clear = StudioBrushes.GhostButton("Remove"); clear.Click += (_, _) => { _session?.SetLogo(null); _dirty = true; }; logoRow.Children.Add(clear); }
                _fields.Children.Add(new TextBlock { Text = "Logo", FontSize = 12, Foreground = StudioBrushes.Text }); _fields.Children.Add(logoRow);
                if (_attached?.Overlays.BrandKit.LogoAssetPath is { Length: > 0 } brandLogo && draft.Image is null) {
                    var useBrand = StudioBrushes.ChipButton("Use the brand kit logo"); useBrand.Click += async (_, _) => await ImportLogoAsync(brandLogo); _fields.Children.Add(useBrand);
                }
            }
            _colors.Children.Add(new TextBlock { Text = "COLORS", FontSize = 10, CharacterSpacing = 160, FontFamily = StudioBrushes.Mono, Foreground = StudioBrushes.Muted, Margin = new Thickness(0, 6, 0, 0) });
            var swatches = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 8 };
            swatches.Children.Add(Swatch("Accent", s => s.AccentColor, (s, c) => s with { AccentColor = c }));
            swatches.Children.Add(Swatch("Background", s => s.BackgroundColor, (s, c) => s with { BackgroundColor = c }));
            swatches.Children.Add(Swatch("Name", s => s.NameColor, (s, c) => s with { NameColor = c }));
            swatches.Children.Add(Swatch("Second line", s => s.TitleColor, (s, c) => s with { TitleColor = c }));
            _colors.Children.Add(swatches);
            var brand = StudioBrushes.ChipButton("Use brand kit colors"); brand.Click += (_, _) => ApplyBrandKit(); _colors.Children.Add(brand);
        } finally { _loading = false; }
    }

    private FrameworkElement FieldControl(NativeGraphicsTemplateField field, JsonElement value)
    {
        var label = Label(field);
        if (field.Choices.Count > 0) {
            var choices = field.Choices;
            var box = new ComboBox { Header = label, ItemsSource = choices.Select(c => c.ValueKind == JsonValueKind.String ? c.GetString() : c.GetRawText()).ToArray(), SelectedIndex = choices.ToList().FindIndex(c => JsonElement.DeepEquals(c, value)), HorizontalAlignment = HorizontalAlignment.Stretch };
            box.SelectionChanged += (_, _) => { if (!_loading && box.SelectedIndex >= 0) Set(field.Id, choices[box.SelectedIndex]); };
            return box;
        }
        switch (field.Type) {
            case "boolean": {
                var toggle = new ToggleSwitch { Header = label, IsOn = value.ValueKind == JsonValueKind.True };
                toggle.Toggled += (_, _) => { if (!_loading) Set(field.Id, JsonSerializer.SerializeToElement(toggle.IsOn)); };
                return toggle;
            }
            case "number": {
                var number = new NumberBox { Header = label, Value = value.ValueKind == JsonValueKind.Number ? value.GetDouble() : double.NaN, Minimum = field.Minimum ?? double.MinValue, Maximum = field.Maximum ?? double.MaxValue, SpinButtonPlacementMode = NumberBoxSpinButtonPlacementMode.Compact, HorizontalAlignment = HorizontalAlignment.Stretch };
                number.ValueChanged += (_, _) => { if (!_loading && !double.IsNaN(number.Value)) Set(field.Id, JsonSerializer.SerializeToElement(number.Value)); };
                return number;
            }
            case "color": {
                var current = value.ValueKind == JsonValueKind.String ? value.GetString() ?? "" : "";
                return SwatchButton(label, current, hex => Set(field.Id, JsonSerializer.SerializeToElement(hex)));
            }
            case "image": {
                var ids = new List<string>(); if (!field.Required) ids.Add("");
                if (_session?.Template?.Definition is { } definition && definition.TryGetProperty("assets", out var assets) && assets.ValueKind == JsonValueKind.Array)
                    foreach (var asset in assets.EnumerateArray()) if (asset.TryGetProperty("id", out var id) && id.GetString() is { } s) ids.Add(s);
                var image = new ComboBox { Header = label, ItemsSource = ids.Select(id => id.Length == 0 ? "None" : id).ToArray(), SelectedIndex = ids.IndexOf(value.ValueKind == JsonValueKind.String ? value.GetString() ?? "" : ""), HorizontalAlignment = HorizontalAlignment.Stretch };
                image.SelectionChanged += (_, _) => { if (!_loading && image.SelectedIndex >= 0) Set(field.Id, JsonSerializer.SerializeToElement(ids[image.SelectedIndex])); };
                return image;
            }
            default: {
                var text = value.ValueKind == JsonValueKind.String ? value.GetString() ?? "" : value.ValueKind is JsonValueKind.Null or JsonValueKind.Undefined ? "" : value.GetRawText();
                var box = new TextBox { Header = label, Text = text, MaxLength = field.MaxLength is > 0 and <= 4096 ? field.MaxLength.Value : 512, AcceptsReturn = field.MaxLength is > 160, TextWrapping = TextWrapping.Wrap };
                box.TextChanged += (_, _) => { if (_loading) return; var next = box.Text; _pendingCommit = () => Set(field.Id, JsonSerializer.SerializeToElement(next)); _debounce.Stop(); _debounce.Start(); };
                box.LostFocus += (_, _) => { if (_pendingCommit is { } commit) { _debounce.Stop(); _pendingCommit = null; commit(); } };
                return box;
            }
        }
    }

    private void Set(string field, JsonElement value) { if (_session is { Busy: false }) { _session.SetField(field, value); _dirty = true; RefreshSaveButton(); } }

    private FrameworkElement Swatch(string label, Func<LowerThirdAppearance, string> read, Func<LowerThirdAppearance, string, LowerThirdAppearance> update)
    {
        var current = _session?.Draft?.Style is { } style ? read(style) : "";
        var swatch = SwatchButton(label, current, hex => { if (_session?.Draft is { } d) { _session.SetStyle(update(d.Style, hex)); _dirty = true; RefreshSaveButton(); } });
        swatch.Tag = (Func<string>)(() => _session?.Draft?.Style is { } s ? read(s) : ""); return swatch;
    }
    private void RefreshColorSwatches()
    {
        foreach (var panel in _colors.Children.OfType<StackPanel>())
            foreach (var swatch in panel.Children.OfType<Button>())
                if (swatch.Tag is Func<string> read && swatch.Content is StackPanel body && body.Children.OfType<Border>().FirstOrDefault() is { } chip && HexColor.TryParse(read(), out var color))
                    chip.Background = new SolidColorBrush(color);
    }
    private static Button SwatchButton(string label, string hex, Action<string> commit)
    {
        var chip = new Border { Width = 22, Height = 22, CornerRadius = new CornerRadius(11), BorderBrush = StudioBrushes.Border, BorderThickness = new Thickness(1),
            Background = HexColor.TryParse(hex, out var color) ? new SolidColorBrush(color) : StudioBrushes.Field };
        var body = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 8 };
        body.Children.Add(chip); body.Children.Add(new TextBlock { Text = label, FontSize = 12, Foreground = StudioBrushes.Text, VerticalAlignment = VerticalAlignment.Center });
        var button = StudioBrushes.GhostButton(""); button.Content = body; button.Padding = new Thickness(8, 4, 10, 4);
        var picker = new ColorPicker { IsAlphaEnabled = false, IsMoreButtonVisible = true, IsHexInputVisible = true, Color = color };
        var flyout = new Flyout { Content = picker, Placement = FlyoutPlacementMode.Bottom };
        picker.ColorChanged += (_, e) => { chip.Background = new SolidColorBrush(e.NewColor); commit(HexColor.ToHex(e.NewColor)); };
        button.Flyout = flyout;
        return button;
    }
    private void ApplyBrandKit()
    {
        if (_session?.Draft is not { } draft || _attached is null) return;
        var kit = _attached.Overlays.BrandKit;
        _session.SetStyle(draft.Style with { AccentColor = kit.AccentColor, BackgroundColor = kit.BackgroundColor, TitleColor = kit.BrandColor });
        _dirty = true; RefreshColorSwatches(); RefreshSaveButton();
    }
    private async Task PickLogoAsync()
    {
        try {
            var picker = new Windows.Storage.Pickers.FileOpenPicker();
            picker.FileTypeFilter.Add(".png"); picker.FileTypeFilter.Add(".svg"); picker.FileTypeFilter.Add(".jpg");
            WinRT.Interop.InitializeWithWindow.Initialize(picker, App.MainWindowHandle);
            var file = await picker.PickSingleFileAsync();
            if (file is not null) await ImportLogoAsync(file.Path);
        } catch (Exception ex) when (ex is not OutOfMemoryException) { _status.Text = "Could not open the logo picker: " + ex.Message; }
    }
    private async Task ImportLogoAsync(string path)
    {
        if (_session is null) return;
        var ok = await _session.ImportLogoAsync(path);
        _status.Text = ok ? "Logo added to this graphic." : _session.ImportStatus;
        if (ok) { _dirty = true; _formSignature = ""; RefreshForm(); RefreshSaveButton(); }
    }

    private void RefreshYours()
    {
        _yours.Children.Clear();
        foreach (var row in Rows) {
            var chip = StudioBrushes.ChipButton(row.Name);
            if (row.Id == _editingRowId) { chip.BorderBrush = StudioBrushes.Accent; chip.BorderThickness = new Thickness(2); }
            ToolTipService.SetToolTip(chip, $"Edit \"{row.Name}\"{(row.Hotkey.Length > 0 ? " (" + row.Hotkey + ")" : "")}");
            chip.Click += (_, _) => LoadRow(row.Id);
            _yours.Children.Add(chip);
        }
        var fresh = StudioBrushes.ChipButton("＋ New"); fresh.Click += (_, _) => StartNew(); _yours.Children.Add(fresh);
        // Hotkey labels say who already owns a key.
        var owners = Rows.Where(r => r.Hotkey.Length > 0 && r.Id != _editingRowId).ToDictionary(r => r.Hotkey, r => r.Name, StringComparer.Ordinal);
        var index = _hotkey.SelectedIndex; _loading = true;
        try { _hotkey.ItemsSource = HotkeyChoices.Select(k => owners.TryGetValue(k, out var who) ? $"{k} · {who}" : k).ToArray(); _hotkey.SelectedIndex = index; } finally { _loading = false; }
    }

    private void RefreshSaveButton()
    {
        var editing = _editingRowId.Length > 0;
        _save.Content = _saving ? "Saving…" : editing ? "Save changes" : "Add to playlist";
        _save.IsEnabled = !_saving && (_dirty || !editing) && (Kind.Id == GraphicsBuilder.SponsorKindId ? _sponsorPath.Length > 0 || editing : _session?.Draft is not null);
        _discard.Content = editing ? "New graphic" : "Start over";
        _previewLine.Text = _session?.PreviewValidation is { Length: > 0 } problem ? problem : _session?.Status is { Length: > 0 } s && s.StartsWith("Waiting", StringComparison.Ordinal) ? s : "Private preview. Nothing here is on air until you Show it from Operate.";
    }

    private void Reflow(double width)
    {
        var narrow = width < 1000; if (narrow == _narrow) return; _narrow = narrow;
        if (narrow) {
            _root.ColumnDefinitions[0].Width = new GridLength(1, GridUnitType.Star); _root.ColumnDefinitions[1].Width = new GridLength(0); _root.ColumnDefinitions[2].Width = new GridLength(0);
            for (var i = _root.RowDefinitions.Count; i < 4; i++) _root.RowDefinitions.Add(new() { Height = GridLength.Auto });
            foreach (var child in _root.Children.OfType<FrameworkElement>()) { if (Grid.GetRow(child) == 0 && Grid.GetColumnSpan(child) == 3) continue; var col = Grid.GetColumn(child); Grid.SetColumn(child, 0); Grid.SetRow(child, 1 + col); }
        } else {
            _root.ColumnDefinitions[0].Width = new GridLength(300); _root.ColumnDefinitions[1].Width = new GridLength(1, GridUnitType.Star); _root.ColumnDefinitions[2].Width = new GridLength(340);
            foreach (var child in _root.Children.OfType<FrameworkElement>()) { if (Grid.GetRow(child) == 0 && Grid.GetColumnSpan(child) == 3) continue; var row = Grid.GetRow(child); Grid.SetRow(child, 1); Grid.SetColumn(child, row - 1); }
        }
    }

    private string SelectedMotionPreset => GraphicsBuilder.MotionPresets[Math.Max(0, _motion.SelectedIndex)].Id;
    private NativeGraphicsAnimations? SelectedMotion => GraphicsBuilder.MotionFor(SelectedMotionPreset, GraphicsBuilder.MotionSpeeds[Math.Max(0, _speed.SelectedIndex)].Ms);
    private void SetMotion(NativeGraphicsAnimations? motion)
    {
        var (preset, ms) = GraphicsBuilder.MotionPresetOf(motion);
        _motion.SelectedIndex = Math.Max(0, GraphicsBuilder.MotionPresets.ToList().FindIndex(m => m.Id == preset));
        var speed = GraphicsBuilder.MotionSpeeds.ToList().FindIndex(m => m.Ms == ms); _speed.SelectedIndex = speed >= 0 ? speed : 1;
        _speed.IsEnabled = preset != "cut";
    }

    // ---- bits ------------------------------------------------------------------------------
    private Button KindTile(GraphicsBuilderKind kind)
    {
        var body = new StackPanel { Spacing = 2 };
        body.Children.Add(new TextBlock { Text = kind.Label, FontSize = 14, FontWeight = Microsoft.UI.Text.FontWeights.SemiBold, Foreground = StudioBrushes.Text });
        body.Children.Add(new TextBlock { Text = kind.Blurb, FontSize = 11, Foreground = StudioBrushes.Muted, TextWrapping = TextWrapping.Wrap });
        var tile = new Button { Content = body, Tag = kind, HorizontalAlignment = HorizontalAlignment.Stretch, HorizontalContentAlignment = HorizontalAlignment.Stretch, Padding = new Thickness(12, 8, 12, 8), CornerRadius = new CornerRadius(8), Background = StudioBrushes.Panel, BorderBrush = StudioBrushes.Border, BorderThickness = new Thickness(1) };
        tile.Click += (_, _) => ChooseKind(kind);
        return tile;
    }
    private static StackPanel Step(string number, string text)
    {
        var step = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 8 };
        step.Children.Add(new Border { Width = 22, Height = 22, CornerRadius = new CornerRadius(11), Background = StudioBrushes.SurfaceRaised, BorderBrush = StudioBrushes.Accent, BorderThickness = new Thickness(1),
            Child = new TextBlock { Text = number, FontSize = 11, FontFamily = new FontFamily("Consolas"), Foreground = StudioBrushes.Accent, HorizontalAlignment = HorizontalAlignment.Center, VerticalAlignment = VerticalAlignment.Center } });
        step.Children.Add(new TextBlock { Text = text, FontSize = 11, CharacterSpacing = 160, FontFamily = StudioBrushes.Mono, Foreground = StudioBrushes.Muted, VerticalAlignment = VerticalAlignment.Center });
        return step;
    }
    private static TextBlock Note(string text) => new() { Text = text, FontSize = 11, Foreground = StudioBrushes.Muted, TextWrapping = TextWrapping.Wrap };
    private static string Label(NativeGraphicsTemplateField field)
    {
        var words = field.Id.Replace('-', ' ').Replace('_', ' ');
        return words.Length == 0 ? "Text" : char.ToUpperInvariant(words[0]) + words[1..];
    }
}
