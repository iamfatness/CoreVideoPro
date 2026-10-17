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
public sealed partial class GraphicsWorkspaceControl : UserControl
{
    public static readonly DependencyProperty StudioProperty = DependencyProperty.Register(nameof(Studio), typeof(StudioViewModel),
        typeof(GraphicsWorkspaceControl), new PropertyMetadata(null, (d, _) => ((GraphicsWorkspaceControl)d).Attach()));
    public StudioViewModel? Studio { get => (StudioViewModel?)GetValue(StudioProperty); set => SetValue(StudioProperty, value); }
    private GraphicsWorkspaceSession? _session;
    private GraphicsAutomationControl? _automation;
    private readonly Expander _automationDrawer=new() {Header="Automation",HorizontalAlignment=HorizontalAlignment.Stretch};
    private GraphicsWorkspaceLibrary? _savedLibrary;
    private StudioViewModel? _attachedStudio;
    private bool _previewSuspended;
    private ColorGradeEditorViewModel? _editor;
    private GradePreviewCoordinator? _previewOwner;
    private readonly Grid _body = new() { ColumnSpacing = 12, RowSpacing = 12 };
    private readonly StackPanel _libraryPanel = new() { Spacing = 8 };
    private StackPanel _inspector => _inspectorTarget;
    private readonly ListView _library = new() { MaxHeight = 260, SelectionMode = ListViewSelectionMode.Single, IsItemClickEnabled=true }, _cues = new() { MaxHeight = 230, SelectionMode = ListViewSelectionMode.Single };
    private readonly ComboBox _libraryView=new() {ItemsSource=new[] {"Cards","Compact list"},SelectedIndex=0,HorizontalAlignment=HorizontalAlignment.Stretch};
    private readonly TextBox _search = new() { PlaceholderText = "Search looks" };
    private readonly ComboBox _scope=new() {Header="New cue scope",HorizontalAlignment=HorizontalAlignment.Stretch};
    private readonly ComboBox _family = new() { ItemsSource = new[] { "All", "lower-third", "title", "bug", "clock", "countdown" }, SelectedIndex = 0, HorizontalAlignment = HorizontalAlignment.Stretch };
    private readonly GraphicsTemplateLayerEditor _layersEditor=new();
    private readonly GraphicsAuthoringCanvas _canvas = new();
    private readonly TextBlock _status = Copy("Choose a look, create a cue, then edit privately."), _native = Copy("Native authoring canvas"), _facts = Copy("");
    private readonly ComboBox _samples=new() {Header="Private preview copy",ItemsSource=new[] {"Your draft","Short text","Long text","Blank secondary","Unicode example"},SelectedIndex=0,HorizontalAlignment=HorizontalAlignment.Stretch};
    private readonly ToggleSwitch _compare = new() { Header = "Compare Original / Edited" };
    private readonly List<Button> _actions = [], _inspectorActions = [];
    private Button _cueButton = null!, _showButton = null!, _updateButton = null!, _hideButton = null!, _deleteButton = null!, _undoButton = null!;
    private Button _packageImportButton = null!;
    private readonly TextBlock _importStatus = Copy("");
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
    private string _thumbnailEpoch="";
    public GraphicsWorkspaceControl()
    {
        BuildWorkspace();
        _libraryView.SelectionChanged+=(_,_)=>RefreshLibrary();
        _search.TextChanged += (_, _) => RefreshLibrary(); _family.SelectionChanged += (_, _) => RefreshLibrary();
        _library.ItemClick += (_,e)=>{var index=_library.Items.IndexOf(e.ClickedItem);if(!_loading && index>=0 && index<_templateRows.Count) _session?.Browse(_templateRows[index]);};
        _library.SelectionChanged += (_, _) => { if (!_loading && _library.SelectedIndex >= 0 && _library.SelectedIndex < _templateRows.Count) _session?.Browse(_templateRows[_library.SelectedIndex]); };
        _cues.SelectionChanged += (_, _) => { if (!_loading && _session is not null && _cues.SelectedIndex >= 0 && _cues.SelectedIndex < _cueIds.Count) _session.Select(_cueIds[_cues.SelectedIndex]); };
        _samples.SelectionChanged+=(_,_)=>{if(!_loading && _samples.SelectedIndex>=0) _session?.SetPreviewSample(new[] {"current","short","long","blank","unicode"}[_samples.SelectedIndex]);};
        _compare.Toggled += (_, _) => { if (!_loading) _session?.SetCompare(_compare.IsOn); };
        Loaded += (_, _) => Attach(); Unloaded += (_, _) => Detach();
        SizeChanged += (_, e) => ApplyWorkspaceSize(e.NewSize.Width);
    }
    private void Attach()
    {
        if (Studio is null || !IsLoaded || _session == Studio.GraphicsWorkspace) return;
        Detach(); _session = Studio.GraphicsWorkspace; _canvas.Session=_session; _layersEditor.Session=_session;
        _automation=new(Studio);_automationDrawer.Content=_automation;
        _attachedStudio = Studio; _attachedStudio.PropertyChanged += OnStudioProperty;
        _savedLibrary = Studio.GraphicsLibrary; _savedLibrary.Changed += OnLibraryChanged; _ = _savedLibrary.LoadAsync();
        _session.Changed += OnChanged; _session.PreviewChanged += OnPreviewChanged;
        _editor = new("preview:lower-third", "Graphics private draft", new() { Lut = "none" }) { LiveEditing = false };
        _editor.PropertyChanged += OnPreviewProperty;
        Refresh(); UpdatePreview();RequestThumbnails();
    }
    private void Detach()
    {
        if (_session is not null) { _session.Changed -= OnChanged; _session.PreviewChanged -= OnPreviewChanged; }
        if (_savedLibrary is not null) _savedLibrary.Changed -= OnLibraryChanged;
        if (_attachedStudio is not null) _attachedStudio.PropertyChanged -= OnStudioProperty;
        _previewOwner?.Dispose(); _previewOwner = null;
        if (_editor is not null) _editor.PropertyChanged -= OnPreviewProperty;
        _canvas.Session=null; _layersEditor.Session=null;
        _automationDrawer.Content=null;_automation=null;
        _session = null; _editor = null; _savedLibrary = null; _attachedStudio = null;
    }
    private void OnPreviewProperty(object? sender, System.ComponentModel.PropertyChangedEventArgs e)
    {
        if (_editor is not null && e.PropertyName == nameof(ColorGradeEditorViewModel.NativeSurface)) {
            _canvas.SurfaceState = _editor.NativeSurface; _native.Text = _editor.NativeSurface.StatusLine;
        }
    }
    private void OnChanged(object? sender, EventArgs e) {Refresh();RequestThumbnails();_automation?.Refresh();}
    private void RequestThumbnails() {
        if(_session is not {Busy:false,State:{CoreEpoch:{Length:>0} epoch}} || _thumbnailEpoch==epoch) return;
        _thumbnailEpoch=epoch;_ = _session.RefreshCatalogAsync();
    }
    private void OnStudioProperty(object? sender, System.ComponentModel.PropertyChangedEventArgs e)
    {
        if (e.PropertyName == nameof(StudioViewModel.IsOverlaysTab)) UpdatePreviewLease();
        if (e.PropertyName is nameof(StudioViewModel.ActiveSceneId) or nameof(StudioViewModel.StudioAssignedLowerThirdLabel) or nameof(StudioViewModel.StudioLowerThirdBusy)) Refresh();
    }
    public void SuspendPreview() { _previewSuspended = true; UpdatePreviewLease(); }
    public void ResumePreview() { _previewSuspended = false; UpdatePreviewLease(); }
    private void UpdatePreviewLease()
    {
        if (!_previewSuspended && Studio?.IsOverlaysTab == true && _session?.Preview is not null && _session.PreviewValidation.Length == 0 && _editor is not null) {
            _previewOwner ??= Studio.CreateAppearancePreview(_editor);
        } else { _previewOwner?.Dispose(); _previewOwner = null; }
    }
    private void OnLibraryChanged(object? sender, EventArgs e) { RefreshSaved(); Refresh(); }
    private void RefreshSaved()
    {
        if (_savedLibrary is null) return;
        _saveStatus.Text = _savedLibrary.Status;
        var key = string.Join('|', _savedLibrary.Document.Cues.Select(c => c.Id + ":" + c.Name)) + ":" + string.Join('|', _savedLibrary.Document.StylePresets.Keys.Concat(_savedLibrary.Document.TemplateStyles.Keys).Distinct(StringComparer.Ordinal));
        if (_savedKey == key) return;
        var selected = _savedCues.SelectedIndex >= 0 && _savedCues.SelectedIndex < _savedIds.Count ? _savedIds[_savedCues.SelectedIndex] : "";
        _savedKey = key; _savedCues.Items.Clear(); _savedIds.Clear();
        foreach (var cue in _savedLibrary.Document.Cues) { _savedIds.Add(cue.Id); _savedCues.Items.Add($"{cue.Name}\n{cue.Draft?.TemplateId} v{cue.Draft?.TemplateVersion} · {cue.Scope}"); }
        _savedCues.SelectedIndex = _savedLibrary.Document.Cues.ToList().FindIndex(c => c.Id == selected);
        _styles.ItemsSource = _savedLibrary.Document.StylePresets.Keys.Concat(_savedLibrary.Document.TemplateStyles.Keys).Distinct(StringComparer.Ordinal).ToArray();
    }
    private void OnPreviewChanged(object? sender, EventArgs e) { Refresh(); UpdatePreview(); }
    private void UpdatePreview()
    {
        if (_editor is null) return;
        if (_session?.Preview is not { } draft) {
            _previewOwner?.Dispose(); _previewOwner = null; _editor.SetNativeUnavailable("Choose a look or cue to preview."); return;
        }
        if (_session.PreviewValidation is { Length: > 0 } invalid) {
            _previewOwner?.Dispose(); _previewOwner = null; _editor.SetNativeUnavailable(invalid); return;
        }
        _editor.SetLowerThirdPreview(new(null, Graphics: draft.ForPreview()));
        UpdatePreviewLease();
    }
    private void Refresh()
    {
        if (_session is null) return;
        _loading = true;
        try {
            _status.Text = _session.Status;
            foreach (var button in _actions) button.IsEnabled = !_session.Busy;
            _packageImportButton.IsEnabled &= !_session.ImportBusy;
            _importStatus.Text = _session.ImportBusy ? _session.ImportStatus : string.Join("\n", (_session.State?.TemplateInstalls ?? []).TakeLast(3)
                .Select(i => "Template import: " + i.State + (i.Reason.Length == 0 ? "" : " — " + i.Reason.Replace('-', ' '))));
            var item = _session.Selected;
            _cueButton.IsEnabled &= item is not null && _session.PreviewSample=="current"; _showButton.IsEnabled &= item is { CueRevision: > 0 };
            _hideButton.IsEnabled &= item is { ProgramRevision: > 0 }; _updateButton.IsEnabled &= item is { ProgramRevision: > 0 } && _session.PreviewSample=="current";
            _deleteButton.IsEnabled &= item is { ProgramRevision: 0 }; _undoButton.IsEnabled &= _session.CanUndo;
            _compare.IsOn = _session.CompareOriginal;
            _samples.SelectedIndex=Array.IndexOf(new[] {"current","short","long","blank","unicode"},_session.PreviewSample);
            _facts.Text = item is null ? "Choose or create a cue. No Program action occurs while browsing." :
                $"{item.TemplateId} v{item.TemplateVersion} · {item.Scope}\nDraft r{item.Revision} / look v{item.TemplateVersion} ({item.Preparation}) · Cue r{item.CueRevision} / look v{item.CueTemplateVersion}{(item.CuePending ? " preparing replacement" : "")} · Program r{item.ProgramRevision} / look v{item.ProgramTemplateVersion} ({item.Visibility}; {(item.CurrentlyComposed ? "draw submitted" : "not composed on latest frame")})\n{item.SourceId} {item.BindingState} {item.Warning} {item.CueWarning}";
            ToolTipService.SetToolTip(_status, _session.Status);
            var scopeKey=string.Join('|',Studio?.Scenes.Select(s=>s.Id+":"+s.Name) ?? []);
            if(_scope.Tag as string!=scopeKey || _scope.Items.Count==0) {
                var prior=(_scope.SelectedItem as ComboBoxItem)?.Tag as string;_scope.Tag=scopeKey;_scope.Items.Clear();
                _scope.Items.Add(new ComboBoxItem {Content="Global — across scenes",Tag=""});
                foreach(var scene in Studio?.Scenes ?? []) _scope.Items.Add(new ComboBoxItem {Content="Scene · "+scene.Name,Tag=scene.Id});
                _scope.SelectedItem=_scope.Items.OfType<ComboBoxItem>().FirstOrDefault(i=>i.Tag as string==prior) ?? _scope.Items[0];
            }
            RefreshLibrary();
            var cueRows = (_session.State?.Instances ?? []).Where(i =>
                (_cueScopeFilter.SelectedIndex != 1 || i.Scope == "global" || i.SceneId == Studio?.ActiveSceneId) &&
                (_cueSearch.Text.Length == 0 || (i.Label + " " + i.Fields.Name + " " + i.TemplateId).Contains(_cueSearch.Text, StringComparison.OrdinalIgnoreCase))).ToArray();
            var key = string.Join('|', cueRows.Select(i => $"{i.InstanceId}:{i.Label}:{i.Fields.Name}:{i.Revision}:{i.RackOrder}:{i.TemplateId}:{i.Visibility}:{i.ProgramRevision}:{i.CueRevision}"));
            if (key != _cueKey) {
                _cueKey = key; _cueIds.Clear(); _cues.Items.Clear();
                foreach (var cue in cueRows) { _cueIds.Add(cue.InstanceId); _cues.Items.Add($"{(cue.Label.Length>0?cue.Label:cue.TemplateId)}\n{cue.Fields.Name} - {(cue.ProgramRevision>0?"On air":cue.CueRevision>0?"Cued":"Draft")}"); }
                _cues.SelectedIndex = _cueIds.IndexOf(_session.SelectedId);
            }
            var inspectorKey = _session.Selected?.ClockConfigurationRevision + ":" + _session.SelectedId + ":" + _session.Draft?.TemplateId + ":" + _session.Draft?.TemplateVersion;
            if (_inspectorKey != inspectorKey) { _inspectorKey = inspectorKey; BuildInspector(); }
            RefreshWorkspaceChrome();
            RefreshLayerList();
            foreach (var refresh in _refreshFields) refresh();
            foreach (var panel in new[] { _contentControls, _styleControls, _layoutControls, _motionControls })
                foreach (var child in panel.Children.OfType<Microsoft.UI.Xaml.Controls.Control>()) child.IsEnabled = !_session.Busy;
        } finally { _loading = false; }
    }
    private void RefreshLibrary()
    {
        if (_session is null) return;
        var loading=_loading;_loading=true;
        try {
            var family=_family.SelectedItem as string ?? "All";
            var templates=(_session.State?.Templates ?? []).Where(t=>(family=="All" || t.Family==family) && (t.Name.Contains(_search.Text,StringComparison.OrdinalIgnoreCase) || t.TemplateId.Contains(_search.Text,StringComparison.OrdinalIgnoreCase))).ToArray();
            var key=_libraryView.SelectedIndex+":"+string.Join('|',templates.Select(t=>$"{t.TemplateId}:{t.TemplateVersion}:{t.Thumbnail?.State}:{t.Thumbnail?.Reason}"));
            var selected=_library.SelectedIndex>=0 && _library.SelectedIndex<_templateRows.Count?_templateRows[_library.SelectedIndex]:null;
            if(key!=_libraryKey) {
                _libraryKey=key;_library.Items.Clear();_templateRows.Clear();
                foreach(var template in templates) {_templateRows.Add(template);_library.Items.Add(new GraphicsTemplateCard(template,_libraryView.SelectedIndex==1));}
            }
            var draft=_session.Selected is null?_session.Draft:null;
            var index=draft is not null?Array.FindIndex(templates,t=>t.TemplateId==draft.TemplateId && t.TemplateVersion==draft.TemplateVersion):-1;
            if(index<0 && selected is not null) index=Array.FindIndex(templates,t=>t.TemplateId==selected.TemplateId && t.TemplateVersion==selected.TemplateVersion);
            _library.SelectedIndex=index;
        } finally {_loading=loading;}
    }
    private void BuildInspector()
    {
        foreach(var button in _inspectorActions) _actions.Remove(button); _inspectorActions.Clear();
        if (_studioFollowSource.Parent is Panel followParent) followParent.Children.Remove(_studioFollowSource);
        foreach (var panel in new[] { _contentControls, _styleControls, _layoutControls, _motionControls }) panel.Children.Clear();
        _inspectorTarget = _contentControls;
        _refreshFields.Clear();
        if (_session?.Draft is not { } draft) { _inspector.Children.Add(Copy("Select a cue to edit its private fields and style.")); return; }
        if(Studio is not null) {var binding=new GraphicsSourceBindingControl(_session,Studio);_inspector.Children.Add(binding);_refreshFields.Add(binding.Refresh);}
        _inspector.Children.Add(new TextBlock { Text = "FIELDS", FontSize = 11 });
        var fields = _session.Template?.Fields ?? [];
        if (fields.Count == 0) fields = [new() { Id = "name", Type = "string", MaxLength = 256 }, new() { Id = "title", Type = "string", MaxLength = 512 }];
        foreach (var field in fields) {
            if (field.Id=="time" && _session.Template?.Family is "clock" or "countdown") {
                var live=Copy("Clock time is generated by the native controller. Configure and start it below."); _inspector.Children.Add(live);
                _refreshFields.Add(()=>live.Text=_session?.Selected?.Clock is { } clock ? $"{clock.Text} · {clock.State}" : "Create a hidden cue to configure its native clock.");
                continue;
            }

            var value = draft.Fields.TryGetProperty(field.Id, out var v) ? v : field.Default ?? JsonSerializer.SerializeToElement("");
            if (!Compatibility && _session.Template?.Layers is { } layers && !HasReference(layers,"field",field.Id)) {
                _inspector.Children.Add(Copy(field.Id + " — this field is not used by the template's layers.")); continue;
            }
            var usages = _session.Template?.Layers is { } numericLayers ? NumericUsages(numericLayers,field.Id) : [];
            IReadOnlyList<JsonElement> choices = field.Choices;
            if (choices.Count == 0 && field.Type == "number") {
                var nativeChoices = usages.Contains("fontWeight") ? new[] {400,500,600,700} : usages.Contains("maxLines") ? new[] {1,2,3} : [];
                choices = nativeChoices.Where(n => n >= (field.Minimum ?? -1e9) && n <= (field.Maximum ?? 1e9)).Select(n => JsonSerializer.SerializeToElement(n)).ToArray();
            }
            if (choices.Count > 0) {
                var choice = new ComboBox { Header = FieldLabel(field), ItemsSource = choices.Select(c => c.ValueKind == JsonValueKind.String ? c.GetString() : c.GetRawText()).ToArray(),
                    SelectedIndex = choices.ToList().FindIndex(c => JsonElement.DeepEquals(c,value)), HorizontalAlignment = HorizontalAlignment.Stretch };
                choice.SelectionChanged += (_,_) => { if (!_loading && choice.SelectedIndex >= 0) _session?.SetField(field.Id,choices[choice.SelectedIndex]); }; _inspector.Children.Add(choice);
                _refreshFields.Add(() => { if (_session?.Draft?.Fields.TryGetProperty(field.Id,out var current) == true) choice.SelectedIndex = choices.ToList().FindIndex(c => JsonElement.DeepEquals(c,current)); });
            } else if (field.Type == "image") {
                var ids = new List<string>(); if (!field.Required) ids.Add("");
                if (_session.Template?.Definition is { } definition && definition.TryGetProperty("assets",out var assets))
                    foreach (var asset in assets.EnumerateArray()) ids.Add(asset.GetProperty("id").GetString()!);
                var image = new ComboBox { Header = FieldLabel(field), ItemsSource = ids.Select(id => id.Length == 0 ? "None" : id).ToArray(),
                    SelectedIndex = ids.IndexOf(value.ValueKind == JsonValueKind.String ? value.GetString() ?? "" : ""), HorizontalAlignment = HorizontalAlignment.Stretch };
                image.SelectionChanged += (_,_) => { if (!_loading && image.SelectedIndex >= 0) _session?.SetField(field.Id,JsonSerializer.SerializeToElement(ids[image.SelectedIndex])); }; _inspector.Children.Add(image);
                _refreshFields.Add(() => { if (_session?.Draft?.Fields.TryGetProperty(field.Id,out var current) == true && current.ValueKind == JsonValueKind.String) image.SelectedIndex = ids.IndexOf(current.GetString() ?? ""); });
            } else if (field.Type == "color") {
                var picker = new ColorPicker { IsAlphaEnabled = field.MaxLength != 7, Color = FieldColor(value) };
                picker.ColorChanged += (_,e) => { if (!_loading) _session?.SetField(field.Id,JsonSerializer.SerializeToElement(HexColor.ToHex(e.NewColor) + (picker.IsAlphaEnabled ? e.NewColor.A.ToString("X2") : ""))); };
                _inspector.Children.Add(new Expander { Header = FieldLabel(field), Content = picker, HorizontalAlignment = HorizontalAlignment.Stretch });
                _refreshFields.Add(() => { if (_session?.Draft?.Fields.TryGetProperty(field.Id,out var current) == true) picker.Color = FieldColor(current); });
            } else if (field.Type == "boolean") {
                var check = new CheckBox { Content = FieldLabel(field), IsChecked = value.ValueKind == JsonValueKind.True };
                check.Click += (_, _) => _session?.SetField(field.Id, JsonSerializer.SerializeToElement(check.IsChecked == true)); _inspector.Children.Add(check);
                _refreshFields.Add(() => { if (_session?.Draft?.Fields.TryGetProperty(field.Id, out var current) == true) check.IsChecked = current.ValueKind == JsonValueKind.True; });
            } else if (field.Type == "number") {
                var limits = usages.Select(NumericLimits).ToArray();
                var minimum = Math.Max(field.Minimum ?? -1e9,limits.Length == 0 ? 0 : limits.Max(l => l.Min));
                var maximum = Math.Min(field.Maximum ?? 1e9,limits.Length == 0 ? 100 : limits.Min(l => l.Max));
                var percent = usages.SetEquals(["opacity"]);
                var pixels = usages.Count > 0 && usages.All(u => u is "x" or "y" or "width" or "height" or "padding" or "gap" or "fontSize" or "cornerRadius");
                var slider = new SliderValueControl { Header = FieldLabel(field), Minimum = minimum, Maximum = maximum,
                    Unit = percent ? "%" : pixels ? "px" : usages.SetEquals(["lineHeight"]) ? "×" : "", DisplayScale = percent ? 100 : 1,
                    DefaultValue = field.Default is { ValueKind: JsonValueKind.Number } dv ? dv.GetDouble() : 0, StepFrequency = maximum-minimum <= 1 ? .01 : maximum-minimum <= 10 ? .05 : 1,
                    Value = value.ValueKind == JsonValueKind.Number ? value.GetDouble() : 0 };
                slider.ValueChanged += (_, number) => _session?.SetField(field.Id, JsonSerializer.SerializeToElement(number)); _inspector.Children.Add(slider);
                slider.EditStarted += (_, _) => _session?.BeginGesture(); slider.EditCompleted += (_, _) => _session?.EndGesture();
                _refreshFields.Add(() => { if (_session?.Draft?.Fields.TryGetProperty(field.Id, out var current) == true && current.ValueKind == JsonValueKind.Number) slider.Value = current.GetDouble(); });
            } else {
                var text = new TextBox { Header = FieldLabel(field) + (field.Required ? " *" : ""), Text = value.ValueKind == JsonValueKind.String ? value.GetString() ?? "" : "", MaxLength = field.MaxLength ?? 512 };
                text.TextChanged += (_, _) => { if (!_loading) _session?.SetField(field.Id, JsonSerializer.SerializeToElement(text.Text)); }; _inspector.Children.Add(text);
                text.GotFocus += (_, _) => _session?.BeginGesture(); text.LostFocus += (_, _) => _session?.EndGesture();
                _refreshFields.Add(() => { if (text.FocusState == FocusState.Unfocused && _session?.Draft?.Fields.TryGetProperty(field.Id, out var current) == true && current.ValueKind == JsonValueKind.String) text.Text = current.GetString() ?? ""; });
            }
        }
        BuildClockControls();
        _inspector.Children.Add(_studioFollowSource);
        if (Compatibility || UsesToken("brand.fontBody") || UsesToken("brand.fontHeading")) {
        _inspectorTarget = _styleControls;
        var font = new TextBox { Header = "Font family", Text = draft.Style.FontFamily, MaxLength = 128 };
        font.LostFocus += (_, _) => { if (_session?.Draft is { } d) _session.SetStyle(d.Style with { FontFamily = font.Text }); }; _inspector.Children.Add(font);
        _refreshFields.Add(() => { if (font.FocusState == FocusState.Unfocused && _session?.Draft is { } d) font.Text = d.Style.FontFamily; });
        }
        _inspectorTarget = _layoutControls;
        var placement = draft.Placement ?? new NativeGraphicsPlacement {
            Anchor=draft.Style.Anchor, X=draft.Style.Anchor.Contains("right",StringComparison.Ordinal) ? -draft.Style.SafeOffsetX*1920 : draft.Style.SafeOffsetX*1920,
            Y=draft.Style.Anchor.Contains("upper",StringComparison.Ordinal) ? draft.Style.SafeOffsetY*1080 : -draft.Style.SafeOffsetY*1080 };
        var anchor = new ComboBox { Header = "Canvas anchor", ItemsSource = NativeGraphicsPlacement.Anchors, SelectedItem = placement.Anchor, HorizontalAlignment = HorizontalAlignment.Stretch };
        anchor.SelectionChanged += (_, _) => { if (!_loading && anchor.SelectedItem is string selected && _session?.Draft is { } d) _session.SetPlacement((d.Placement ?? placement) with { Anchor=selected }); }; _inspector.Children.Add(anchor);
        _refreshFields.Add(() => { if (_session?.Draft is { } d) anchor.SelectedItem = (d.Placement ?? placement).Anchor; });
        AddPlacement("Horizontal offset",-3840,3840,0,1,"px",1,placement,p=>p.X,(p,v)=>p with { X=v });
        AddPlacement("Vertical offset",-2160,2160,0,1,"px",1,placement,p=>p.Y,(p,v)=>p with { Y=v });
        AddPlacement("Scale",.1,4,1,.01,"%",100,placement,p=>p.Scale,(p,v)=>p with { Scale=v });
        _inspectorTarget = _styleControls;
        if (_session.Template?.Runtime != "declarative") {
            var image=new GraphicsImageControl(_session);_inspector.Children.Add(image);_refreshFields.Add(image.Refresh);
            AddStyle("Logo scale", .5, 2, 1, .05, "%", 100, d => d.LogoScale, (d,n) => d with {LogoScale=n});
            AddStyle("Width", .15, .95, .6, .01, "%", 100, d => d.Width, (d, n) => d with { Width = n });
            AddStyle("Name size", 16, 120, 42, 1, "px", 1, d => d.NameSize, (d, n) => d with { NameSize = n });
            AddStyle("Secondary size", 12, 80, 28, 1, "px", 1, d => d.TitleSize, (d, n) => d with { TitleSize = n });
            AddStyle("Padding", 0, 64, 20, 1, "px", 1, d => d.Padding, (d, n) => d with { Padding = n });
            AddStyle("Corners", 0, 60, 8, 1, "px", 1, d => d.CornerRadius, (d, n) => d with { CornerRadius = n });
        }
        if (Compatibility || UsesToken("brand.primary")) AddStyle("Background opacity", 0, 1, .9, .01, "%", 100, d => d.BackgroundOpacity, (d, n) => d with { BackgroundOpacity = n });
        if (Compatibility || UsesToken("brand.onPrimary") || UsesToken("brand.foreground")) AddColor("Name / foreground", d => d.NameColor, (d, c) => d with { NameColor = c });
        if (Compatibility || UsesToken("brand.onPrimaryMuted") || UsesToken("brand.secondary")) AddColor("Secondary foreground", d => d.TitleColor, (d, c) => d with { TitleColor = c });
        if (Compatibility || UsesToken("brand.primary") || UsesToken("brand.background")) AddColor("Plate", d => d.BackgroundColor, (d, c) => d with { BackgroundColor = c });
        if (Compatibility || UsesToken("brand.accent")) AddColor("Accent", d => d.AccentColor, (d, c) => d with { AccentColor = c });
        BuildMotionControls();
    }
    private static ComboBox ClockChoice(string header,string value,params (string Id,string Label)[] choices)
    {
        var box=new ComboBox { Header=header,HorizontalAlignment=HorizontalAlignment.Stretch,SelectedValuePath="Tag" };
        foreach(var choice in choices) box.Items.Add(new ComboBoxItem { Content=choice.Label,Tag=choice.Id });
        box.SelectedValue=value; return box;
    }
    private void BuildClockControls()
    {
        if (_session?.Selected is not { } item || _session.Template?.Family is not ("clock" or "countdown")) return;
        var current=item.ClockRequestedConfiguration ?? item.Clock?.Configuration ?? new NativeGraphicsClockConfiguration();
        var panel=new StackPanel { Spacing=8 };
        var mode=ClockChoice("Clock mode",current.Mode,("wall","Wall clock"),("countdown","Countdown"));
        var zone=ClockChoice("Wall-clock timezone",current.Zone,("local","Computer local time"),("utc","UTC"),("fixed","UTC with fixed offset"));
        var format=ClockChoice("Wall-clock display",current.Format,("hms","Hours : minutes : seconds"),("hm","Hours : minutes"));
        var offset=new SliderValueControl { Header="UTC offset",Minimum=-840,Maximum=840,StepFrequency=15,Unit="min",Value=current.OffsetMinutes };
        var duration=new SliderValueControl { Header="Countdown duration",Minimum=1,Maximum=86400,StepFrequency=1,Unit="s",Value=current.DurationSeconds };
        var finish=ClockChoice("At zero",current.Finish,("hold-zero","Hold at zero"),("hide","Hide graphic"),("suggest-next","Suggest next cue"));
        foreach(var control in new UIElement[] { mode,zone,format,offset,duration,finish }) panel.Children.Add(control);
        panel.Children.Add(Copy("Applying configuration pauses and resets the countdown. Start never shows the graphic. Suggest next is a status, never an automatic Take."));
        panel.Children.Add(InspectorAction("Apply clock settings",async()=>{ if(_session is not null) await _session.ClockAsync("configure",new() { Mode=mode.SelectedValue as string ?? "wall",Zone=zone.SelectedValue as string ?? "local",Format=format.SelectedValue as string ?? "hms",Finish=finish.SelectedValue as string ?? "hold-zero",OffsetMinutes=(int)offset.Value,DurationSeconds=(int)duration.Value },expectedClockRevision:item.ClockConfigurationRevision); }));
        var transport=new StackPanel { Orientation=Orientation.Horizontal,Spacing=6 };
        foreach(var command in new[] { "start","pause","resume","reset" }) transport.Children.Add(InspectorAction(char.ToUpperInvariant(command[0])+command[1..],async()=>{if(_session is not null) await _session.ClockAsync(command);}));
        panel.Children.Add(transport); _inspector.Children.Add(new Expander { Header="Native clock / countdown",Content=panel,HorizontalAlignment=HorizontalAlignment.Stretch });
    }
    private void AddPlacement(string label,double min,double max,double neutral,double step,string unit,double displayScale,
        NativeGraphicsPlacement fallback,Func<NativeGraphicsPlacement,double> read,Func<NativeGraphicsPlacement,double,NativeGraphicsPlacement> update)
    {
        var slider=new SliderValueControl { Header=label,Minimum=min,Maximum=max,DefaultValue=neutral,StepFrequency=step,Unit=unit,DisplayScale=displayScale,Value=read(_session?.Draft?.Placement ?? fallback) };
        slider.ValueChanged += (_,value) => { if (!_loading && _session?.Draft is { } d) _session.SetPlacement(update(d.Placement ?? fallback,value)); };
        slider.EditStarted += (_,_)=>_session?.BeginGesture(); slider.EditCompleted += (_,_)=>_session?.EndGesture();
        _inspector.Children.Add(slider); _refreshFields.Add(()=>slider.Value=read(_session?.Draft?.Placement ?? fallback));
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
    private void AddColor(string label, Func<LowerThirdAppearance,string> read, Func<LowerThirdAppearance, string, LowerThirdAppearance> update)
    {
        var color = _session?.Draft is { } draft ? read(draft.Style) : "#FFFFFF";
        var picker = new ColorPicker { IsAlphaEnabled = false, Color = HexColor.ParseOrDefault(color, Windows.UI.Color.FromArgb(255, 255, 255, 255)) };
        picker.ColorChanged += (_, e) => { if (!_loading && _session?.Draft is { } d) _session.SetStyle(update(d.Style, HexColor.ToHex(e.NewColor))); };
        _inspector.Children.Add(new Expander { Header = label, Content = picker, HorizontalAlignment = HorizontalAlignment.Stretch });
        _refreshFields.Add(() => { if (_session?.Draft is { } d) picker.Color = HexColor.ParseOrDefault(read(d.Style),picker.Color); });
    }
    private bool Compatibility => _session?.Template?.Runtime != "declarative";
    private bool UsesToken(string token) => _session?.Template?.Layers is { } layers && HasReference(layers,"token",token);
    private static bool HasReference(JsonElement node,string key,string value)
    {
        if (node.ValueKind == JsonValueKind.Object) return (node.TryGetProperty(key,out var field) && field.ValueKind == JsonValueKind.String && field.GetString() == value) || node.EnumerateObject().Any(p => p.Name != "metadata" && HasReference(p.Value,key,value));
        return node.ValueKind == JsonValueKind.Array && node.EnumerateArray().Any(child => HasReference(child,key,value));
    }
    private static Windows.UI.Color FieldColor(JsonElement value)
    {
        var text = value.ValueKind == JsonValueKind.String ? value.GetString() ?? "" : "";
        return GraphicsColor.Parse(text,Windows.UI.Color.FromArgb(255,255,255,255));
    }
    private static string FieldLabel(NativeGraphicsTemplateField field)
    {
        if (field.ExtensionData?.TryGetValue("metadata",out var metadata) == true && metadata.ValueKind == JsonValueKind.Object &&
            metadata.TryGetProperty("label",out var label) && label.ValueKind == JsonValueKind.String && label.GetString() is { Length: > 0 and <= 128 } friendly) return friendly;
        var text = field.Id.Replace('-',' '); return text.Length == 0 ? text : char.ToUpperInvariant(text[0])+text[1..];
    }
    private static HashSet<string> NumericUsages(JsonElement node,string field)
    {
        var result = new HashSet<string>(StringComparer.Ordinal);
        void Visit(JsonElement value) {
            if (value.ValueKind == JsonValueKind.Object) foreach (var property in value.EnumerateObject()) {
                if (property.Name == "metadata") continue;
                if (property.Value.ValueKind == JsonValueKind.Object && property.Value.TryGetProperty("field",out var reference) && reference.ValueKind == JsonValueKind.String && reference.GetString() == field) result.Add(property.Name);
                Visit(property.Value);
            } else if (value.ValueKind == JsonValueKind.Array) foreach (var child in value.EnumerateArray()) Visit(child);
        }
        Visit(node); return result;
    }
    private static (double Min,double Max) NumericLimits(string property) => property switch {
        "x" or "y" => (0,8192), "width" or "height" => (1,8192), "opacity" => (0,1), "padding" or "gap" => (0,256),
        "fontSize" => (1,512), "lineHeight" => (.5,3), "cornerRadius" => (0,4096), _ => (-1e9,1e9)
    };
    private Button InspectorAction(string label,Func<Task> action)
    {
        var button=Action(label,action); _inspectorActions.Add(button); return button;
    }
    private Button Action(string label, Func<Task> action)
    {
        var button = ChromeButton(label); button.Click += async (_, _) => await action(); _actions.Add(button); return button;
    }
    private async Task PickPackageAsync()
    {
        var session = _session; if (session is null) return;
        try {
            var picker = new Windows.Storage.Pickers.FileOpenPicker();
            picker.FileTypeFilter.Add(".zip"); picker.FileTypeFilter.Add(".cvgraphics");
            WinRT.Interop.InitializeWithWindow.Initialize(picker, App.MainWindowHandle);
            var file = await picker.PickSingleFileAsync();
            if (file is not null) await session.ImportPackageAsync(file.Path);
        } catch (Exception ex) when (ex is not OutOfMemoryException) { _importStatus.Text = "Package import unavailable: " + ex.Message; }
    }
    private async Task ExportPackageAsync()
    {
        var template=_session?.Template;
        if(template is not {Runtime:"declarative"} || _session?.Draft?.Scratch is not null) {_importStatus.Text="Select an installed declarative template and save private template edits as a version before exporting.";return;}
        try {
            var picker=new Windows.Storage.Pickers.FileSavePicker {SuggestedFileName=template.TemplateId+"-v"+template.TemplateVersion};
            picker.FileTypeChoices.Add("CoreVideo graphics package",new List<string> {".cvgraphics"});WinRT.Interop.InitializeWithWindow.Initialize(picker,App.MainWindowHandle);
            var file=await picker.PickSaveFileAsync();if(file is null) return;
            await GraphicsPackageWriter.WriteAsync(template,file.Path);_importStatus.Text="Exact template version and verified images exported. Program and the private draft are unchanged.";
        } catch(Exception ex) when(ex is not OutOfMemoryException) {_importStatus.Text="Package export unavailable: "+ex.Message;}
    }
    private static TextBlock Copy(string value) => new() { Text = value, TextWrapping = TextWrapping.Wrap, FontSize = 12 };
}
