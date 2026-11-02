using System.Text.Json;
using CoreVideoPro.MediaCore.Models;
using CoreVideoPro.MediaCore.Services;
using CoreVideoPro.WinUI.ViewModels;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Automation;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Media;

namespace CoreVideoPro.WinUI.Controls;

/// <summary>Graphics Designer slice 2 (spec mockup D1): the screen a design opens in. Header (back to Your designs,
/// Undo, Save, Save to playlist · F-key, status), the layers panel, the native canvas with snapping and sample-text
/// buttons, and the inspector (Layer / Variables). Every edit goes through the workspace session's private draft
/// (one undo step each; slider drags are one gesture). The decisions live in MediaCore
/// (<see cref="GraphicsDesignerModel"/>, <see cref="GraphicsTemplateEditor"/>, <see cref="GraphicsSnap"/>); this
/// control only lays them out. Editing is paused while a save runs.</summary>
public sealed partial class GraphicsDesignerControl : UserControl
{
    public static readonly DependencyProperty StudioProperty = DependencyProperty.Register(nameof(Studio), typeof(StudioViewModel),
        typeof(GraphicsDesignerControl), new PropertyMetadata(null, (d, _) => ((GraphicsDesignerControl)d).Attach()));
    public StudioViewModel? Studio { get => (StudioViewModel?)GetValue(StudioProperty); set => SetValue(StudioProperty, value); }

    private const double WideBreakpoint = 1500;
    private readonly Grid _root = new() { RowSpacing = 10 }, _body = new() { ColumnSpacing = 12 };
    private readonly GraphicsAuthoringCanvas _canvas = new() { SnapToGuides = true };
    private readonly TextBlock _family = new() { FontSize = 11, CharacterSpacing = 120, FontWeight = Microsoft.UI.Text.FontWeights.SemiBold, Foreground = StudioBrushes.Muted, TextTrimming = TextTrimming.CharacterEllipsis },
        _designName = new() { FontSize = 18, FontWeight = Microsoft.UI.Text.FontWeights.SemiBold, Foreground = StudioBrushes.Text, TextTrimming = TextTrimming.CharacterEllipsis },
        _status = new() { FontSize = 13, Foreground = StudioBrushes.Text, TextWrapping = TextWrapping.Wrap, MaxLines = 2, TextTrimming = TextTrimming.CharacterEllipsis, VerticalAlignment = VerticalAlignment.Center };
    private readonly Button _back = StudioBrushes.GhostButton("← Your designs"), _undo = StudioBrushes.GhostButton("Undo"),
        _save = StudioBrushes.GhostButton("Save"), _saveToPlaylist = StudioBrushes.AccentButton("Save to playlist");
    private readonly StackPanel _samples = new() { Orientation = Orientation.Horizontal, Spacing = 6, VerticalAlignment = VerticalAlignment.Center };
    private readonly List<(string Id, Button Button)> _sampleButtons = [];

    private GraphicsWorkspaceSession? _session; private StudioViewModel? _attached;
    private ColorGradeEditorViewModel? _editor; private IDisposable? _previewOwner; private bool _suspended = true;
    private bool _saving, _loading;

    public GraphicsDesignerControl()
    {
        _root.RowDefinitions.Add(new() { Height = GridLength.Auto }); _root.RowDefinitions.Add(new() { Height = new GridLength(1, GridUnitType.Star) });
        _root.Children.Add(BuildHeader());
        _body.ColumnDefinitions.Add(new() { Width = new GridLength(240) });
        _body.ColumnDefinitions.Add(new() { Width = new GridLength(1, GridUnitType.Star) });
        _body.ColumnDefinitions.Add(new() { Width = new GridLength(300) });
        var layers = BuildLayersPanel(); _body.Children.Add(layers);
        var centre = BuildCentre(); Grid.SetColumn(centre, 1); _body.Children.Add(centre);
        var inspector = BuildInspector(); Grid.SetColumn(inspector, 2); _body.Children.Add(inspector);
        Grid.SetRow(_body, 1); _root.Children.Add(_body);
        _root.Background = StudioBrushes.Background; _root.Padding = new Thickness(0, 4, 0, 0);
        Content = _root;
        Loaded += (_, _) => Attach(); Unloaded += (_, _) => Detach();
        SizeChanged += (_, e) => Reflow(e.NewSize.Width);
    }

    private Grid BuildHeader()
    {
        var header = new Grid { ColumnSpacing = 14 };
        foreach (var width in new[] { GridLength.Auto, GridLength.Auto, new GridLength(1, GridUnitType.Star), GridLength.Auto }) header.ColumnDefinitions.Add(new() { Width = width });
        _back.VerticalAlignment = VerticalAlignment.Center; AutomationProperties.SetName(_back, "Back to Your designs");
        ToolTipService.SetToolTip(_back, "Back to Your designs. Unsaved edits stay in this private draft.");
        _back.Click += (_, _) => { if (_attached is { } s) s.GraphicsDesignerOpen = false; };
        header.Children.Add(_back);
        var title = new StackPanel { Spacing = 0, VerticalAlignment = VerticalAlignment.Center, MaxWidth = 360 }; title.Children.Add(_family); title.Children.Add(_designName);
        Grid.SetColumn(title, 1); header.Children.Add(title);
        AutomationProperties.SetName(_status, "Design status"); Grid.SetColumn(_status, 2); header.Children.Add(_status);
        var actions = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 8, VerticalAlignment = VerticalAlignment.Center };
        _save.Foreground = StudioBrushes.Accent; _save.BorderBrush = StudioBrushes.Accent;
        foreach (var button in new[] { _undo, _save, _saveToPlaylist }) { button.FontSize = 14; button.MinHeight = 36; actions.Children.Add(button); }
        AutomationProperties.SetName(_undo, "Undo"); AutomationProperties.SetName(_save, "Save design"); AutomationProperties.SetName(_saveToPlaylist, "Save to playlist");
        ToolTipService.SetToolTip(_undo, "Undo the last change to this draft.");
        ToolTipService.SetToolTip(_save, "Store your edits as the design's next version. Program and Preview are unchanged.");
        ToolTipService.SetToolTip(_saveToPlaylist, "Save, then put this design in the playlist on the F-key shown. Show it from Operate.");
        _undo.Click += (_, _) => { if (!_saving) _session?.Undo(); };
        _save.Click += async (_, _) => await SaveAsync(toPlaylist: false);
        _saveToPlaylist.Click += async (_, _) => await SaveAsync(toPlaylist: true);
        Grid.SetColumn(actions, 3); header.Children.Add(actions);
        return header;
    }

    private Grid BuildCentre()
    {
        var centre = new Grid { RowSpacing = 8 };
        centre.RowDefinitions.Add(new() { Height = new GridLength(1, GridUnitType.Star) }); centre.RowDefinitions.Add(new() { Height = GridLength.Auto });
        centre.Children.Add(new Border { Background = StudioBrushes.Field, BorderBrush = StudioBrushes.Border, BorderThickness = new Thickness(1), CornerRadius = new CornerRadius(8), Padding = new Thickness(6), Child = _canvas });
        var bar = new Grid { ColumnSpacing = 10 };
        bar.ColumnDefinitions.Add(new() { Width = GridLength.Auto }); bar.ColumnDefinitions.Add(new() { Width = GridLength.Auto }); bar.ColumnDefinitions.Add(new() { Width = new GridLength(1, GridUnitType.Star) });
        bar.Children.Add(new TextBlock { Text = "Sample text:", FontSize = 14, Foreground = StudioBrushes.Muted, VerticalAlignment = VerticalAlignment.Center });
        foreach (var (id, label) in GraphicsPreviewSamples.Buttons) {
            var button = StudioBrushes.GhostButton(label); button.FontSize = 14; button.FontWeight = Microsoft.UI.Text.FontWeights.Normal;
            AutomationProperties.SetName(button, label);
            button.Click += (_, _) => _session?.SetPreviewSample(id);
            _sampleButtons.Add((id, button)); _samples.Children.Add(button);
        }
        ToolTipService.SetToolTip(_samples, "Preview only: Typical is your draft's values; Longest name and Empty title test the layout. Drag edits work on Typical.");
        Grid.SetColumn(_samples, 1); bar.Children.Add(_samples);
        var caption = new TextBlock { Text = "Safe area · snap to guides on · 1920 × 1080 · hold Ctrl to place freely", FontSize = 13, Foreground = StudioBrushes.Muted,
            TextWrapping = TextWrapping.Wrap, MaxLines = 2, TextTrimming = TextTrimming.CharacterEllipsis, HorizontalAlignment = HorizontalAlignment.Right, TextAlignment = TextAlignment.Right, VerticalAlignment = VerticalAlignment.Center };
        Grid.SetColumn(caption, 2); bar.Children.Add(caption);
        Grid.SetRow(bar, 1); centre.Children.Add(bar);
        return centre;
    }

    private static Border Panel(UIElement child) => new() { Background = StudioBrushes.Panel, BorderBrush = StudioBrushes.Border, BorderThickness = new Thickness(1), CornerRadius = new CornerRadius(8), Padding = new Thickness(12), Child = child };
    private static TextBlock Heading(string text) => new() { Text = text, FontSize = 13, FontWeight = Microsoft.UI.Text.FontWeights.Bold, CharacterSpacing = 80, Foreground = StudioBrushes.Muted, VerticalAlignment = VerticalAlignment.Center, TextTrimming = TextTrimming.CharacterEllipsis };

    private void Reflow(double width)
    {
        var wide = width >= WideBreakpoint;
        _body.ColumnDefinitions[0].Width = new GridLength(wide ? 240 : 220);
        _body.ColumnDefinitions[2].Width = new GridLength(wide ? 300 : 280);
    }

    // ---- lifecycle -------------------------------------------------------------------------
    private void Attach()
    {
        if (Studio is null || !IsLoaded || ReferenceEquals(_attached, Studio)) return;
        Detach();
        _attached = Studio; _session = Studio.GraphicsWorkspace;
        _canvas.Session = _session; _variables.Session = _session;
        _session.Changed += OnSessionChanged; _session.PreviewChanged += OnSessionChanged;
        _attached.PropertyChanged += OnStudioProperty; _attached.GraphicsDesignOpened += OnDesignOpened;
        _attached.GraphicsLibrary.Changed += OnLibraryChanged;
        _editor = new("preview:lower-third", "Graphics designer preview", new() { Lut = "none" }) { LiveEditing = false };
        _editor.PropertyChanged += OnEditorProperty;
        RefreshAll(); UpdatePreviewLease();
    }
    private void Detach()
    {
        if (_session is not null) { _session.Changed -= OnSessionChanged; _session.PreviewChanged -= OnSessionChanged; }
        if (_attached is not null) { _attached.PropertyChanged -= OnStudioProperty; _attached.GraphicsDesignOpened -= OnDesignOpened; _attached.GraphicsLibrary.Changed -= OnLibraryChanged; }
        if (_editor is not null) _editor.PropertyChanged -= OnEditorProperty;
        _previewOwner?.Dispose(); _previewOwner = null; _canvas.Session = null; _variables.Session = null;
        _session = null; _attached = null; _editor = null; _rowsSignature = _inspectorSignature = "";
    }
    /// <summary>The page holds the private preview only while this is the visible Design surface.</summary>
    public void SuspendPreview() { _suspended = true; UpdatePreviewLease(); }
    public void ResumePreview() { _suspended = false; UpdatePreviewLease(); RefreshAll(); }
    private void UpdatePreviewLease()
    {
        if (_editor is null) return;
        if (_session?.Preview is not { } draft) { _previewOwner?.Dispose(); _previewOwner = null; _editor.SetNativeUnavailable("Open a design to preview it."); return; }
        if (_session.PreviewValidation is { Length: > 0 } invalid) { _previewOwner?.Dispose(); _previewOwner = null; _editor.SetNativeUnavailable(invalid); return; }
        if (!_suspended && _attached?.IsOverlaysTab == true) {
            _editor.SetLowerThirdPreview(new(null, Graphics: draft.ForPreview()));
            _previewOwner ??= _attached.CreateAppearancePreview(_editor);
        } else { _previewOwner?.Dispose(); _previewOwner = null; }
    }
    private void OnEditorProperty(object? sender, System.ComponentModel.PropertyChangedEventArgs e)
    {
        if (_editor is not null && e.PropertyName == nameof(ColorGradeEditorViewModel.NativeSurface)) _canvas.SurfaceState = _editor.NativeSurface;
    }
    private void OnStudioProperty(object? sender, System.ComponentModel.PropertyChangedEventArgs e)
    {
        if (e.PropertyName is nameof(StudioViewModel.ActiveTab) or nameof(StudioViewModel.IsOverlaysTab)) UpdatePreviewLease();
        else if (e.PropertyName == nameof(StudioViewModel.IsGraphicsDesignerVisible)) RefreshAll();
    }
    private void OnLibraryChanged(object? sender, EventArgs e) => RefreshHeader();
    private void OnSessionChanged(object? sender, EventArgs e) { RefreshAll(); UpdatePreviewLease(); }
    private void OnDesignOpened(object? sender, EventArgs e)
    {
        _status.Text = "Design open. Your edits stay private until you Save."; SelectTab(layerTab: true); RefreshAll();
    }

    // ---- refresh ---------------------------------------------------------------------------
    private bool Visible => _attached?.IsGraphicsDesignerVisible == true;
    private bool CanEdit => !_saving && _session is { Busy: false, Draft.Declarative: true } && _session.TemplateDocument is not null;

    private void RefreshAll()
    {
        if (_session is null || _loading) return;
        _loading = true;
        try {
            var rows = CurrentRows();
            // The inspector always shows a layer: keep the selection on an existing row (front-most when none).
            if (Visible && !_saving && !_session.Busy && rows.Count > 0 && rows.All(r => r.Id != _session.SelectedLayerId)) _session.SelectLayer(rows[0].Id);
            RefreshHeader(); RefreshRows(rows); RefreshInspector(rows); RefreshSamples();
            _canvas.IsEnabled = !_saving;
            if (_variablesScroll.Visibility == Visibility.Visible) _variables.Refresh();
            _variablesScroll.IsEnabled = !_saving;
        } finally { _loading = false; }
    }

    private void RefreshHeader()
    {
        if (_session is null) return;
        var draft = _session.Draft; var design = _attached?.GraphicsDesignFor(draft);
        var family = _session.Template?.Family ?? design?.Family ?? "";
        _family.Text = (family.Length == 0 ? "GRAPHIC" : family.Replace('-', ' ').ToUpperInvariant()) + " · DRAFT";
        _designName.Text = design?.Name ?? _session.Template?.Name ?? "No design open";
        var hotkey = design is not null && _attached is { } studio ? studio.GraphicsDesignPlaylistHotkey(design) : "";
        _saveToPlaylist.Content = _saving ? "Saving…" : hotkey.Length > 0 ? "Save to playlist · " + hotkey : "Save to playlist";
        _save.Content = design is null ? "Save as new design" : "Save";
        _save.IsEnabled = _saveToPlaylist.IsEnabled = !_saving && _session is { Busy: false, Draft.Declarative: true };
        _undo.IsEnabled = !_saving && _session is { Busy: false, CanUndo: true };
        _back.IsEnabled = !_saving;
        ToolTipService.SetToolTip(_status, _status.Text.Length == 0 ? null : _status.Text);
    }

    private void RefreshSamples()
    {
        if (_session is null) return;
        foreach (var (id, button) in _sampleButtons) {
            var on = _session.PreviewSample == id;
            button.BorderBrush = on ? StudioBrushes.Accent : StudioBrushes.Border; button.Foreground = on ? StudioBrushes.Accent : StudioBrushes.Text;
            button.IsEnabled = !_saving && _session.Draft is not null;
        }
    }

    /// <summary>Show a message in the header status line (edit refusals, save results, moves).</summary>
    private void Say(string message) { _status.Text = message; ToolTipService.SetToolTip(_status, message.Length == 0 ? null : message); }

    // ---- edits -----------------------------------------------------------------------------
    /// <summary>One private template edit: the change plus draft values matched to the edited variables (an added clock or
    /// image variable gets its default). A refusal leaves the draft untouched and lands in the status line.</summary>
    private bool Apply(Func<NativeGraphicsScratch, NativeGraphicsScratch> change, string? select = null, string? done = null)
    {
        if (_session is not { } session) return false;
        if (_saving) { Say("Editing is paused while the design saves."); return false; }
        if (session.Busy) { Say("Graphics is busy. Try again in a moment."); return false; }
        if (session.Draft is not { Declarative: true } draft || session.TemplateDocument is not { } document) { Say("Open a design to edit it."); return false; }
        NativeGraphicsScratch edited; JsonElement fields;
        try { edited = change(document); fields = GraphicsTemplateEditor.SyncFields(draft.Fields, edited); }
        catch (Exception ex) when (ex is InvalidDataException or InvalidOperationException or JsonException or ArgumentException) { Say(ex.Message); return false; }
        session.Edit(draft with { Scratch = edited.Hash == session.Template?.ManifestHash ? null : edited, Fields = fields });
        if (select is not null) session.SelectLayer(select);
        if (done is not null) Say(done);
        return true;
    }

    /// <summary>Numbers go through the session so a variable-bound one edits the draft value; a refusal lands in the status line.</summary>
    private void SetNumbers(string id, IReadOnlyDictionary<string, double> values)
    {
        if (_session is not { } session) return;
        if (_saving) { Say("Editing is paused while the design saves."); return; }
        var before = session.Draft; var status = session.Status;
        session.SetLayerNumbers(id, values);
        if (ReferenceEquals(before, session.Draft) && session.Status != status) Say(session.Status);
    }

    private async Task SaveAsync(bool toPlaylist)
    {
        if (_attached is not { } studio || _saving) return;
        CommitPendingText();
        _saving = true;
        Say((toPlaylist ? "Saving the design and adding it to the playlist…" : "Saving the design…") + " Editing is paused until it finishes.");
        RefreshAll();
        try {
            var result = toPlaylist ? await studio.SaveGraphicsDesignToPlaylistAsync() : await studio.SaveGraphicsDesignAsync();
            Say(result.Message);
        } catch (Exception ex) when (ex is not OutOfMemoryException) { Say("The design was not saved: " + ex.Message); }
        finally { _saving = false; _rowsSignature = _inspectorSignature = ""; RefreshAll(); }
    }
}
