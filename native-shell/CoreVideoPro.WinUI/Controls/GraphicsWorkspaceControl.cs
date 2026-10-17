using System.Text.Json;
using CoreVideoPro.MediaCore.Models;
using CoreVideoPro.MediaCore.Services;
using CoreVideoPro.WinUI.Services;
using CoreVideoPro.WinUI.ViewModels;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Media;

namespace CoreVideoPro.WinUI.Controls;

/// <summary>Native Graphics operator workspace. This view never renders media,
/// controls Program by editing a form, or replaces the existing legacy controls.</summary>
public sealed class GraphicsWorkspaceControl : UserControl
{
    public static readonly DependencyProperty StudioProperty = DependencyProperty.Register(nameof(Studio), typeof(StudioViewModel),
        typeof(GraphicsWorkspaceControl), new PropertyMetadata(null, (d, _) => ((GraphicsWorkspaceControl)d).Attach()));
    public StudioViewModel? Studio { get => (StudioViewModel?)GetValue(StudioProperty); set => SetValue(StudioProperty, value); }
    private GraphicsWorkspaceSession? _session;
    private GraphicsWorkspaceLibrary? _savedLibrary;
    private StudioViewModel? _attachedStudio;
    private bool _previewSuspended;
    private ColorGradeEditorViewModel? _editor;
    private GradePreviewCoordinator? _previewOwner;
    private readonly Grid _body = new() { ColumnSpacing = 12, RowSpacing = 12 };
    private readonly StackPanel _libraryPanel = new() { Spacing = 8 }, _inspector = new() { Spacing = 10 };
    private readonly ListView _library = new() { MaxHeight = 260, SelectionMode = ListViewSelectionMode.Single }, _cues = new() { MaxHeight = 230, SelectionMode = ListViewSelectionMode.Single };
    private readonly TextBox _search = new() { PlaceholderText = "Search looks" };
    private readonly ComboBox _family = new() { ItemsSource = new[] { "All", "lower-third", "title", "bug", "clock", "countdown" }, SelectedIndex = 0, HorizontalAlignment = HorizontalAlignment.Stretch };
    private readonly VideoSurfaceHost _canvas = new() { Height = 330 };
    private readonly TextBlock _status = Copy("Choose a look, create a cue, then edit privately."), _native = Copy("Native authoring canvas"), _facts = Copy("");
    private readonly ToggleSwitch _compare = new() { Header = "Compare Original / Edited" };
    private readonly List<Button> _actions = [];
    private readonly Button _cueButton, _showButton, _updateButton, _hideButton, _deleteButton, _undoButton;
    private readonly List<NativeGraphicsTemplate> _templateRows = [];
    private readonly List<string> _cueIds = [];
    private readonly List<string> _savedIds = [];
    private readonly ListView _savedCues = new() { MaxHeight = 220 };
    private readonly TextBox _saveName = new() { Header = "Save selected cue as", MaxLength = 128, PlaceholderText = "Interview — guest" };
    private readonly ComboBox _styles = new() { Header = "Saved style overrides", HorizontalAlignment = HorizontalAlignment.Stretch };
    private readonly TextBox _styleName = new() { Header = "Save style as", MaxLength = 80 };
    private readonly TextBlock _saveStatus = Copy("");
    private readonly List<System.Action> _refreshFields = [];
    private string _inspectorKey = "", _libraryKey = "", _cueKey = "", _savedKey = "";
    private bool _loading;
    public GraphicsWorkspaceControl()
    {
        _body.ColumnDefinitions.Add(new() { Width = new GridLength(250) });
        _body.ColumnDefinitions.Add(new() { Width = new GridLength(1, GridUnitType.Star), MinWidth = 320 });
        _body.ColumnDefinitions.Add(new() { Width = new GridLength(300) });
        _body.RowDefinitions.Add(new() { Height = GridLength.Auto });
        _body.RowDefinitions.Add(new() { Height = GridLength.Auto });
        var heading = new StackPanel { Spacing = 5 };
        heading.Children.Add(new TextBlock { Text = "Graphics", FontSize = 22, FontWeight = Microsoft.UI.Text.FontWeights.SemiBold });
        heading.Children.Add(Copy("Private draft → accepted Cue → explicit Show. Video Takes remain separate."));
        heading.Children.Add(_status); Grid.SetColumnSpan(heading, 3); _body.Children.Add(heading);
        _libraryPanel.Children.Add(new TextBlock { Text = "LIBRARY", FontSize = 11, FontWeight = Microsoft.UI.Text.FontWeights.SemiBold });
        _libraryPanel.Children.Add(_search); _libraryPanel.Children.Add(_family); _libraryPanel.Children.Add(_library);
        _libraryPanel.Children.Add(Action("Create hidden cue", async () => { if (_library.SelectedIndex >= 0 && _session is not null) await _session.CreateAsync(_templateRows[_library.SelectedIndex]); }));
        _libraryPanel.Children.Add(new TextBlock { Text = "CUE RACK", FontSize = 11, Margin = new Thickness(0, 12, 0, 0) });
        _libraryPanel.Children.Add(_cues); _deleteButton = Action("Delete hidden cue", async () => { if (_session is not null) await _session.RemoveAsync(); }); _libraryPanel.Children.Add(_deleteButton);
        _libraryPanel.Children.Add(Action("Duplicate into hidden cue", async () => { if (_session is not null) await _session.DuplicateAsync(); }));
        _libraryPanel.Children.Add(_saveName);
        _libraryPanel.Children.Add(Action("Save cue", async () => { if (_savedLibrary is not null) await _savedLibrary.SaveCueAsync(_saveName.Text); }));
        _libraryPanel.Children.Add(new TextBlock { Text = "SAVED CUES", FontSize = 11, Margin = new Thickness(0, 12, 0, 0) });
        _libraryPanel.Children.Add(_savedCues);
        _libraryPanel.Children.Add(Action("Restore as hidden cue", async () => { if (_savedLibrary is not null && _savedCues.SelectedIndex >= 0) await _savedLibrary.RestoreAsync(_savedLibrary.Document.Cues[_savedCues.SelectedIndex]); }));
        var savedActions = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 6 };
        savedActions.Children.Add(Action("Move up", async () => { if (_savedLibrary is not null && _savedCues.SelectedIndex >= 0) await _savedLibrary.MoveSavedAsync(_savedLibrary.Document.Cues[_savedCues.SelectedIndex].Id, -1); }));
        savedActions.Children.Add(Action("Move down", async () => { if (_savedLibrary is not null && _savedCues.SelectedIndex >= 0) await _savedLibrary.MoveSavedAsync(_savedLibrary.Document.Cues[_savedCues.SelectedIndex].Id, 1); }));
        _libraryPanel.Children.Add(savedActions);
        _libraryPanel.Children.Add(Action("Delete saved copy", async () => { if (_savedLibrary is not null && _savedCues.SelectedIndex >= 0) await _savedLibrary.RemoveSavedAsync(_savedLibrary.Document.Cues[_savedCues.SelectedIndex].Id); }));
        _libraryPanel.Children.Add(_saveStatus);
        var import = new TextBox { Header = "Import declarative template", PlaceholderText = "Paste template.json", AcceptsReturn = true, Height = 110, MaxLength = 262144 };
        var importPanel = new StackPanel { Spacing = 8 }; importPanel.Children.Add(import);
        importPanel.Children.Add(Copy("Local declarations only. Validation is asynchronous; the previous version and on-air graphics remain intact."));
        importPanel.Children.Add(Action("Validate and import", async () => { if (_session is not null) await _session.ImportAsync(import.Text); }));
        _libraryPanel.Children.Add(new Expander { Header = "Import template", Content = importPanel, HorizontalAlignment = HorizontalAlignment.Stretch });
        var libraryScroll = new ScrollViewer { Content = _libraryPanel, HorizontalScrollBarVisibility = ScrollBarVisibility.Disabled, VerticalScrollBarVisibility = ScrollBarVisibility.Auto, MaxHeight = 780 };
        var libraryDrawer = new Expander { Header = "Library and cue rack", Content = libraryScroll, IsExpanded = true, HorizontalAlignment = HorizontalAlignment.Stretch, HorizontalContentAlignment = HorizontalAlignment.Stretch };
        Grid.SetRow(libraryDrawer, 1); _body.Children.Add(libraryDrawer);
        var center = new StackPanel { Spacing = 10 };
        center.Children.Add(_facts); center.Children.Add(_canvas); center.Children.Add(_native); center.Children.Add(_compare);
        var transport = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 8 };
        _cueButton = Action("Cue to Preview", async () => { if (_session is not null) await _session.CueAsync(); });
        _showButton = Action("Show on Program", async () => { if (_session is not null) await _session.ShowAsync(); });
        _hideButton = Action("Hide", async () => { if (_session is not null) await _session.HideAsync(); });
        transport.Children.Add(_cueButton); transport.Children.Add(_showButton); transport.Children.Add(_hideButton); center.Children.Add(transport);
        var editActions = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 8 };
        _updateButton = Action("Update on air", async () => { if (_session is not null) await _session.UpdateProgramAsync(); });
        _undoButton = Action("Undo draft", () => { _session?.Undo(); return Task.CompletedTask; });
        editActions.Children.Add(_updateButton); editActions.Children.Add(_undoButton);
        editActions.Children.Add(Action("Cancel draft", () => { _session?.CancelDraft(); return Task.CompletedTask; })); center.Children.Add(editActions);
        center.Children.Add(Action("Clear Program graphics", async () => { if (_session is not null) await _session.ClearAsync(); }));
        center.Children.Add(Copy("Clear removes managed Volt graphics. Captions, clean source recordings and webcam-only guides keep their own controls."));
        var savedStyles = new StackPanel { Spacing = 8 }; savedStyles.Children.Add(_styles); savedStyles.Children.Add(_styleName);
        savedStyles.Children.Add(Action("Save style preset", async () => { if (_savedLibrary is not null) await _savedLibrary.SaveStyleAsync(_styleName.Text); }));
        savedStyles.Children.Add(Action("Use selected saved style in draft", () => { if (_styles.SelectedItem is string name) _savedLibrary?.ApplyStyle(name); return Task.CompletedTask; }));
        savedStyles.Children.Add(Action("Use current brand look in draft", () => { if (Studio is not null) _session?.SetStyle(Studio.Overlays.AppliedAppearance); return Task.CompletedTask; }));
        center.Children.Add(new Expander { Header = "Style presets", Content = savedStyles, HorizontalAlignment = HorizontalAlignment.Stretch });
        Grid.SetColumn(center, 1); Grid.SetRow(center, 1); _body.Children.Add(center);
        var inspectorScroll = new ScrollViewer { Content = _inspector, HorizontalScrollBarVisibility = ScrollBarVisibility.Disabled, VerticalScrollBarVisibility = ScrollBarVisibility.Auto, MaxHeight = 780 };
        var inspectorDrawer = new Expander { Header = "Selected cue controls", Content = inspectorScroll, IsExpanded = true, HorizontalAlignment = HorizontalAlignment.Stretch, HorizontalContentAlignment = HorizontalAlignment.Stretch };
        Grid.SetColumn(inspectorDrawer, 2); Grid.SetRow(inspectorDrawer, 1); _body.Children.Add(inspectorDrawer);
        Content = _body;
        _search.TextChanged += (_, _) => RefreshLibrary(); _family.SelectionChanged += (_, _) => RefreshLibrary();
        _cues.SelectionChanged += (_, _) => { if (!_loading && _session is not null && _cues.SelectedIndex >= 0 && _cues.SelectedIndex < _cueIds.Count) _session.Select(_cueIds[_cues.SelectedIndex]); };
        _compare.Toggled += (_, _) => { if (!_loading) _session?.SetCompare(_compare.IsOn); };
        Loaded += (_, _) => Attach(); Unloaded += (_, _) => Detach();
        bool? wasNarrow = null;
        SizeChanged += (_, e) => {
            // Stack the same selected panels on small windows; never force an
            // endless horizontal strip or discard an edit to change layout.
            var narrow = e.NewSize.Width < 1000;
            if (wasNarrow == narrow) return;
            wasNarrow = narrow;
            libraryDrawer.IsExpanded = inspectorDrawer.IsExpanded = !narrow;
            while (_body.RowDefinitions.Count < 4) _body.RowDefinitions.Add(new() { Height = GridLength.Auto });
            Grid.SetColumn(libraryDrawer, 0); Grid.SetRow(libraryDrawer, narrow ? 2 : 1); Grid.SetColumnSpan(libraryDrawer, narrow ? 3 : 1);
            Grid.SetColumn(inspectorDrawer, narrow ? 0 : 2); Grid.SetRow(inspectorDrawer, narrow ? 3 : 1); Grid.SetColumnSpan(inspectorDrawer, narrow ? 3 : 1);
            Grid.SetColumn(center, narrow ? 0 : 1); Grid.SetColumnSpan(center, narrow ? 3 : 1);
            _body.ColumnDefinitions[0].Width = narrow ? new GridLength(0) : new GridLength(250);
            _body.ColumnDefinitions[2].Width = narrow ? new GridLength(0) : new GridLength(300);
        };
    }
    private void Attach()
    {
        if (Studio is null || !IsLoaded || _session == Studio.GraphicsWorkspace) return;
        Detach(); _session = Studio.GraphicsWorkspace;
        _attachedStudio = Studio; _attachedStudio.PropertyChanged += OnStudioProperty;
        _savedLibrary = Studio.GraphicsLibrary; _savedLibrary.Changed += OnLibraryChanged; _ = _savedLibrary.LoadAsync();
        _session.Changed += OnChanged; _session.PreviewChanged += OnPreviewChanged;
        _editor = new("preview:lower-third", "Graphics private draft", new() { Lut = "none" }) { LiveEditing = false };
        _editor.PropertyChanged += OnPreviewProperty;
        Refresh(); UpdatePreview();
    }
    private void Detach()
    {
        if (_session is not null) { _session.Changed -= OnChanged; _session.PreviewChanged -= OnPreviewChanged; }
        if (_savedLibrary is not null) _savedLibrary.Changed -= OnLibraryChanged;
        if (_attachedStudio is not null) _attachedStudio.PropertyChanged -= OnStudioProperty;
        _previewOwner?.Dispose(); _previewOwner = null;
        if (_editor is not null) _editor.PropertyChanged -= OnPreviewProperty;
        _session = null; _editor = null; _savedLibrary = null; _attachedStudio = null;
    }
    private void OnPreviewProperty(object? sender, System.ComponentModel.PropertyChangedEventArgs e)
    {
        if (_editor is not null && e.PropertyName == nameof(ColorGradeEditorViewModel.NativeSurface)) {
            _canvas.SurfaceState = _editor.NativeSurface; _native.Text = _editor.NativeSurface.StatusLine;
        }
    }
    private void OnChanged(object? sender, EventArgs e) => Refresh();
    private void OnStudioProperty(object? sender, System.ComponentModel.PropertyChangedEventArgs e)
    {
        if (e.PropertyName == nameof(StudioViewModel.IsOverlaysTab)) UpdatePreviewLease();
    }
    public void SuspendPreview() { _previewSuspended = true; UpdatePreviewLease(); }
    public void ResumePreview() { _previewSuspended = false; UpdatePreviewLease(); }
    private void UpdatePreviewLease()
    {
        if (!_previewSuspended && Studio?.IsOverlaysTab == true && _session?.Preview is not null && _editor is not null) {
            _previewOwner ??= Studio.CreateAppearancePreview(_editor);
        } else { _previewOwner?.Dispose(); _previewOwner = null; }
    }
    private void OnLibraryChanged(object? sender, EventArgs e) { RefreshSaved(); Refresh(); }
    private void RefreshSaved()
    {
        if (_savedLibrary is null) return;
        _saveStatus.Text = _savedLibrary.Status;
        var key = string.Join('|', _savedLibrary.Document.Cues.Select(c => c.Id + ":" + c.Name)) + ":" + string.Join('|', _savedLibrary.Document.StylePresets.Keys);
        if (_savedKey == key) return;
        var selected = _savedCues.SelectedIndex >= 0 && _savedCues.SelectedIndex < _savedIds.Count ? _savedIds[_savedCues.SelectedIndex] : "";
        _savedKey = key; _savedCues.Items.Clear(); _savedIds.Clear();
        foreach (var cue in _savedLibrary.Document.Cues) { _savedIds.Add(cue.Id); _savedCues.Items.Add($"{cue.Name}\n{cue.Draft?.TemplateId} v{cue.Draft?.TemplateVersion} · {cue.Scope}"); }
        _savedCues.SelectedIndex = _savedLibrary.Document.Cues.ToList().FindIndex(c => c.Id == selected);
        _styles.ItemsSource = _savedLibrary.Document.StylePresets.Keys.ToArray();
    }
    private void OnPreviewChanged(object? sender, EventArgs e) { Refresh(); UpdatePreview(); }
    private void UpdatePreview()
    {
        if (_editor is null || _session?.Preview is not { } draft) return;
        _editor.SetLowerThirdPreview(new(null, Graphics: draft.ForPreview()));
        UpdatePreviewLease();
    }
    private void Refresh()
    {
        if (_session is null) return;
        _loading = true;
        try {
            _status.Text = _session.Status;
            foreach (var button in _actions) button.IsEnabled = !_session.Busy && _savedLibrary?.Busy != true;
            var item = _session.Selected;
            _cueButton.IsEnabled &= item is not null; _showButton.IsEnabled &= item is { CueRevision: > 0 };
            _hideButton.IsEnabled &= item is { ProgramRevision: > 0 }; _updateButton.IsEnabled &= item is { ProgramRevision: > 0 };
            _deleteButton.IsEnabled &= item is { ProgramRevision: 0 }; _undoButton.IsEnabled &= _session.CanUndo;
            _compare.IsOn = _session.CompareOriginal;
            _facts.Text = item is null ? "Choose or create a cue. No Program action occurs while browsing." :
                $"{item.TemplateId} v{item.TemplateVersion} · {item.Scope}\nDraft r{item.Revision} ({item.Preparation}) · Cue r{item.CueRevision}{(item.CuePending ? " preparing replacement" : "")} · Program r{item.ProgramRevision} ({item.Visibility})\n{item.SourceId} {item.BindingState} {item.Warning} {item.CueWarning}";
            RefreshLibrary();
            var key = string.Join('|', _session.State?.Instances.Select(i => $"{i.InstanceId}:{i.TemplateId}:{i.Visibility}:{i.ProgramRevision}:{i.CueRevision}") ?? []);
            if (key != _cueKey) {
                _cueKey = key; _cueIds.Clear(); _cues.Items.Clear();
                foreach (var cue in _session.State?.Instances ?? []) { _cueIds.Add(cue.InstanceId); _cues.Items.Add($"{cue.TemplateId} · {cue.Fields.Name}\n{cue.Visibility} · Cue {cue.CueRevision} / Program {cue.ProgramRevision}"); }
                _cues.SelectedIndex = _cueIds.IndexOf(_session.SelectedId);
            }
            var inspectorKey = _session.SelectedId + ":" + _session.Draft?.TemplateId + ":" + _session.Draft?.TemplateVersion;
            if (_inspectorKey != inspectorKey) { _inspectorKey = inspectorKey; BuildInspector(); }
            foreach (var refresh in _refreshFields) refresh();
            foreach (var child in _inspector.Children.OfType<Microsoft.UI.Xaml.Controls.Control>()) child.IsEnabled = !_session.Busy;
        } finally { _loading = false; }
    }
    private void RefreshLibrary()
    {
        if (_session is null) return;
        var family = _family.SelectedItem as string ?? "All";
        var templates = (_session.State?.Templates ?? []).Where(t => (family == "All" || t.Family == family) &&
            (t.Name.Contains(_search.Text, StringComparison.OrdinalIgnoreCase) || t.TemplateId.Contains(_search.Text, StringComparison.OrdinalIgnoreCase))).ToArray();
        var key = string.Join('|', templates.Select(t => $"{t.TemplateId}:{t.TemplateVersion}"));
        if (key == _libraryKey) return;
        var selected = _library.SelectedIndex >= 0 && _library.SelectedIndex < _templateRows.Count ? _templateRows[_library.SelectedIndex] : null;
        _libraryKey = key; _library.Items.Clear(); _templateRows.Clear();
        foreach (var template in templates) { _templateRows.Add(template); _library.Items.Add($"{template.Name}\n{template.Family} · v{template.TemplateVersion}"); }
        _library.SelectedIndex = selected is null ? (templates.Length > 0 ? 0 : -1) : Array.FindIndex(templates, t => t.TemplateId == selected.TemplateId && t.TemplateVersion == selected.TemplateVersion);
    }
    private void BuildInspector()
    {
        _inspector.Children.Clear();
        _refreshFields.Clear();
        if (_session?.Draft is not { } draft) { _inspector.Children.Add(Copy("Select a cue to edit its private fields and style.")); return; }
        _inspector.Children.Add(new TextBlock { Text = "FIELDS", FontSize = 11 });
        var fields = _session.Template?.Fields ?? [];
        if (fields.Count == 0) fields = [new() { Id = "name", Type = "string", MaxLength = 256 }, new() { Id = "title", Type = "string", MaxLength = 512 }];
        foreach (var field in fields) {
            var value = draft.Fields.TryGetProperty(field.Id, out var v) ? v : field.Default ?? JsonSerializer.SerializeToElement("");
            if (field.Type == "boolean") {
                var check = new CheckBox { Content = field.Id, IsChecked = value.ValueKind == JsonValueKind.True };
                check.Click += (_, _) => _session?.SetField(field.Id, JsonSerializer.SerializeToElement(check.IsChecked == true)); _inspector.Children.Add(check);
                _refreshFields.Add(() => { if (_session?.Draft?.Fields.TryGetProperty(field.Id, out var current) == true) check.IsChecked = current.ValueKind == JsonValueKind.True; });
            } else if (field.Type == "number") {
                var slider = new SliderValueControl { Header = field.Id, Minimum = field.Minimum ?? 0, Maximum = field.Maximum ?? 100,
                    DefaultValue = field.Default is { ValueKind: JsonValueKind.Number } dv ? dv.GetDouble() : 0, StepFrequency = 1,
                    Value = value.ValueKind == JsonValueKind.Number ? value.GetDouble() : 0 };
                slider.ValueChanged += (_, number) => _session?.SetField(field.Id, JsonSerializer.SerializeToElement(number)); _inspector.Children.Add(slider);
                slider.EditStarted += (_, _) => _session?.BeginGesture(); slider.EditCompleted += (_, _) => _session?.EndGesture();
                _refreshFields.Add(() => { if (_session?.Draft?.Fields.TryGetProperty(field.Id, out var current) == true && current.ValueKind == JsonValueKind.Number) slider.Value = current.GetDouble(); });
            } else {
                var text = new TextBox { Header = field.Id + (field.Required ? " *" : ""), Text = value.ValueKind == JsonValueKind.String ? value.GetString() ?? "" : "", MaxLength = field.MaxLength ?? 512 };
                text.TextChanged += (_, _) => { if (!_loading) _session?.SetField(field.Id, JsonSerializer.SerializeToElement(text.Text)); }; _inspector.Children.Add(text);
                text.GotFocus += (_, _) => _session?.BeginGesture(); text.LostFocus += (_, _) => _session?.EndGesture();
                _refreshFields.Add(() => { if (text.FocusState == FocusState.Unfocused && _session?.Draft?.Fields.TryGetProperty(field.Id, out var current) == true && current.ValueKind == JsonValueKind.String) text.Text = current.GetString() ?? ""; });
            }
        }
        _inspector.Children.Add(Copy("An empty secondary line stays empty. Source text remains available in Sources."));
        var font = new TextBox { Header = "Font family", Text = draft.Style.FontFamily, MaxLength = 128 };
        font.LostFocus += (_, _) => { if (_session?.Draft is { } d) _session.SetStyle(d.Style with { FontFamily = font.Text }); }; _inspector.Children.Add(font);
        _refreshFields.Add(() => { if (font.FocusState == FocusState.Unfocused && _session?.Draft is { } d) font.Text = d.Style.FontFamily; });
        var anchor = new ComboBox { Header = "Anchor", ItemsSource = CaptionStyleHelper.LowerThirdPositionOptions, SelectedItem = draft.Style.Anchor, HorizontalAlignment = HorizontalAlignment.Stretch };
        anchor.SelectionChanged += (_, _) => { if (!_loading && anchor.SelectedItem is string selected && _session?.Draft is { } d) _session.SetStyle(d.Style with { Anchor = selected }); }; _inspector.Children.Add(anchor);
        _refreshFields.Add(() => { if (_session?.Draft is { } d) anchor.SelectedItem = d.Style.Anchor; });
        AddStyle("Side safe offset", 0, .25, .05, .005, "%", 100, d => d.SafeOffsetX, (d, n) => d with { SafeOffsetX = n });
        AddStyle("Top / bottom safe offset", 0, .25, .06, .005, "%", 100, d => d.SafeOffsetY, (d, n) => d with { SafeOffsetY = n });
        if (_session.Template?.Runtime != "declarative") {
            AddStyle("Width", .15, .95, .6, .01, "%", 100, d => d.Width, (d, n) => d with { Width = n });
            AddStyle("Name size", 16, 120, 42, 1, "px", 1, d => d.NameSize, (d, n) => d with { NameSize = n });
            AddStyle("Secondary size", 12, 80, 28, 1, "px", 1, d => d.TitleSize, (d, n) => d with { TitleSize = n });
            AddStyle("Padding", 0, 64, 20, 1, "px", 1, d => d.Padding, (d, n) => d with { Padding = n });
            AddStyle("Corners", 0, 60, 8, 1, "px", 1, d => d.CornerRadius, (d, n) => d with { CornerRadius = n });
        }
        AddStyle("Background opacity", 0, 1, .9, .01, "%", 100, d => d.BackgroundOpacity, (d, n) => d with { BackgroundOpacity = n });
        AddColor("Name / foreground", draft.Style.NameColor, (d, c) => d with { NameColor = c });
        AddColor("Secondary foreground", draft.Style.TitleColor, (d, c) => d with { TitleColor = c });
        AddColor("Plate", draft.Style.BackgroundColor, (d, c) => d with { BackgroundColor = c });
        AddColor("Accent", draft.Style.AccentColor, (d, c) => d with { AccentColor = c });
        _inspector.Children.Add(Copy("Colors, fonts and placement affect this draft. Cue or Update on air sends an explicit native revision."));
    }
    private void AddStyle(string label, double min, double max, double neutral, double step, string unit, double scale,
        Func<LowerThirdAppearance, double> read, Func<LowerThirdAppearance, double, LowerThirdAppearance> update)
    {
        if (_session?.Draft is not { } draft) return;
        var slider = new SliderValueControl { Header = label, Minimum = min, Maximum = max, DefaultValue = neutral, StepFrequency = step, Unit = unit, DisplayScale = scale, Value = read(draft.Style) };
        slider.ValueChanged += (_, value) => { if (!_loading && _session?.Draft is { } d) _session.SetStyle(update(d.Style, value)); }; _inspector.Children.Add(slider);
        slider.EditStarted += (_, _) => _session?.BeginGesture(); slider.EditCompleted += (_, _) => _session?.EndGesture();
        _refreshFields.Add(() => { if (_session?.Draft is { } d) slider.Value = read(d.Style); });
    }
    private void AddColor(string label, string color, Func<LowerThirdAppearance, string, LowerThirdAppearance> update)
    {
        var picker = new ColorPicker { IsAlphaEnabled = false, Color = HexColor.ParseOrDefault(color, Windows.UI.Color.FromArgb(255, 255, 255, 255)) };
        picker.ColorChanged += (_, e) => { if (!_loading && _session?.Draft is { } d) _session.SetStyle(update(d.Style, HexColor.ToHex(e.NewColor))); };
        _inspector.Children.Add(new Expander { Header = label, Content = picker, HorizontalAlignment = HorizontalAlignment.Stretch });
    }
    private Button Action(string label, Func<Task> action)
    {
        var button = new Button { Content = label }; button.Click += async (_, _) => await action(); _actions.Add(button); return button;
    }
    private static TextBlock Copy(string value) => new() { Text = value, TextWrapping = TextWrapping.Wrap, FontSize = 12 };
}
