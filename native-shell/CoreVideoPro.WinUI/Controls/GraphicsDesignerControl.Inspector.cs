using System.Text.Json;
using CoreVideoPro.MediaCore.Models;
using CoreVideoPro.MediaCore.Services;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Automation;
using Microsoft.UI.Xaml.Controls;

namespace CoreVideoPro.WinUI.Controls;

/// <summary>The Designer's inspector: <c>{LABEL} · {KIND} LAYER</c>, then Layer (per-kind properties from
/// <see cref="GraphicsDesignerModel.Inspect"/>) and Variables (<see cref="GraphicsVariablesPanel"/>) tabs. A locked layer's
/// inputs are disabled (rename and lock stay in the layers panel).</summary>
public sealed partial class GraphicsDesignerControl
{
    private readonly TextBlock _inspectorTitle = Heading("");
    private readonly Button _layerTab = StudioBrushes.GhostButton("Layer"), _variablesTab = StudioBrushes.GhostButton("Variables");
    private readonly StackPanel _layerBody = new() { Spacing = 6, Padding = new Thickness(0, 0, 3, 4) };
    private readonly ScrollViewer _layerScroll = new() { VerticalScrollBarVisibility = ScrollBarVisibility.Auto, HorizontalScrollBarVisibility = ScrollBarVisibility.Disabled };
    private readonly ScrollViewer _variablesScroll = new() { VerticalScrollBarVisibility = ScrollBarVisibility.Auto, HorizontalScrollBarVisibility = ScrollBarVisibility.Disabled, Visibility = Visibility.Collapsed };
    private readonly GraphicsVariablesPanel _variables = new();
    private readonly List<Action<GraphicsLayerInspection>> _inspectorRefresh = [];
    private readonly List<Microsoft.UI.Xaml.Controls.Control> _inspectorInputs = [];
    private string _inspectorSignature = "";

    private Border BuildInspector()
    {
        var grid = new Grid { RowSpacing = 5 };
        foreach (var height in new[] { GridLength.Auto, GridLength.Auto, new GridLength(1, GridUnitType.Star) }) grid.RowDefinitions.Add(new() { Height = height });
        grid.Children.Add(_inspectorTitle);
        var tabs = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 3 };
        foreach (var tab in new[] { _layerTab, _variablesTab, _motionTab }) { CompactButton(tab); tabs.Children.Add(tab); }
        AutomationProperties.SetName(_layerTab, "Layer tab"); AutomationProperties.SetName(_variablesTab, "Variables tab");
        AutomationProperties.SetName(_motionTab, "Motion tab");
        _motionTab.Click += (_, _) => { CommitPendingText(); SelectTab(layerTab: false, motionTab: true); };
        _layerTab.Click += (_, _) => SelectTab(layerTab: true); _variablesTab.Click += (_, _) => SelectTab(layerTab: false);
        Grid.SetRow(tabs, 1); grid.Children.Add(tabs);
        _layerScroll.Content = _layerBody; AutomationProperties.SetName(_layerScroll, "Layer properties");
        _variablesScroll.Content = _variables; AutomationProperties.SetName(_variablesScroll, "Design variables");
        _motionScroll.Content = _motionPanel; AutomationProperties.SetName(_motionScroll, "Layer motion");
        var bodies = new Grid(); bodies.Children.Add(_layerScroll); bodies.Children.Add(_variablesScroll); bodies.Children.Add(_motionScroll);
        Grid.SetRow(bodies, 2); grid.Children.Add(bodies);
        SelectTab(layerTab: true);
        return Panel(grid);
    }

    private void SelectTab(bool layerTab, bool motionTab = false)
    {
        _layerScroll.Visibility = layerTab ? Visibility.Visible : Visibility.Collapsed;
        _variablesScroll.Visibility = layerTab || motionTab ? Visibility.Collapsed : Visibility.Visible;
        _motionScroll.Visibility = motionTab ? Visibility.Visible : Visibility.Collapsed;
        _layerTab.BorderBrush = layerTab ? StudioBrushes.Accent : StudioBrushes.Border; _layerTab.Foreground = layerTab ? StudioBrushes.Accent : StudioBrushes.Text;
        var variables = !layerTab && !motionTab;
        _variablesTab.BorderBrush = variables ? StudioBrushes.Accent : StudioBrushes.Border;
        _variablesTab.Foreground = variables ? StudioBrushes.Accent : StudioBrushes.Text;
        _motionTab.BorderBrush = motionTab ? StudioBrushes.Accent : StudioBrushes.Border;
        _motionTab.Foreground = motionTab ? StudioBrushes.Accent : StudioBrushes.Text;
        if (variables) _variables.Refresh();
        RefreshAll();
    }

    private void CommitPendingText() => _pending.Flush();

    private void RefreshInspector(IReadOnlyList<GraphicsDesignerRow> rows)
    {
        if (_session is not { } session || _motionScroll.Visibility == Visibility.Visible) return;
        var document = session.Draft is { Declarative: true } ? session.TemplateDocument : null;
        var row = rows.FirstOrDefault(r => r.Id == session.SelectedLayerId);
        GraphicsLayerInspection? inspection = null;
        if (document is not null && row is not null) {
            try { inspection = GraphicsDesignerModel.Inspect(document, session.Draft!.Fields, row.Id); }
            catch (Exception ex) when (ex is InvalidDataException or JsonException) { Say(ex.Message); }
        }
        _inspectorTitle.Text = inspection is null ? "NO LAYER SELECTED" : $"{inspection.Label.ToUpperInvariant()} · {inspection.Kind.ToUpperInvariant()} LAYER";
        IReadOnlyList<NativeGraphicsTemplateField> fields = [];
        if (document is not null) { try { fields = GraphicsTemplateEditor.TemplateFields(document); } catch (Exception ex) when (ex is InvalidDataException or JsonException) { } }
        var signature = inspection is null ? "" : $"{inspection.Id}:{inspection.Kind}:{inspection.ShowsVariable is null}:{inspection.FontChoice}:{inspection.ColourChoice}:{inspection.FillChoice}:{inspection.ImageSource}:"
            + string.Join(',', fields.Select(f => f.Id + "/" + f.Type))
            + (document is null ? "" : ContentSignature(GraphicsTemplateEditor.Layer(document, inspection.Id)));
        if (signature != _inspectorSignature) { _inspectorSignature = signature; BuildLayerInspector(inspection, fields, document); }
        if (inspection is not null) foreach (var refresh in _inspectorRefresh) refresh(inspection);
        var editable = CanEdit && inspection is { Locked: false };
        foreach (var input in _inspectorInputs) input.IsEnabled = editable;
        _layerScroll.IsEnabled = !_saving;
    }

    private void BuildLayerInspector(GraphicsLayerInspection? layer, IReadOnlyList<NativeGraphicsTemplateField> fields, NativeGraphicsScratch? document)
    {
        _layerBody.Children.Clear(); _inspectorRefresh.Clear(); _inspectorInputs.Clear(); _pending.Clear(); // RefreshAll flushed text typed for another layer first
        if (layer is null) { _layerBody.Children.Add(Note("Choose a layer in the list or on the canvas.")); return; }
        var id = layer.Id;
        var content = document is null ? default : GraphicsTemplateEditor.Layer(document, id);
        if (layer.Locked) _layerBody.Children.Add(Note("This layer is locked. Unlock it in the layers list to edit it."));
        switch (layer.Kind) {
            case "Text" or "Clock": BuildText(layer, fields); break;
            case "Shape" or "Ellipse": BuildShape(layer); break;
            case "Image": BuildImage(layer, fields, document); break;
            case "Crawl": BuildCrawl(layer, content); break;
        }
        var position = new Grid { ColumnSpacing = 3 };
        var boxes = new (string Property, string Label, double Min, Func<GraphicsLayerInspection, double> Read)[] {
            ("x", "X", 0, l => l.X), ("y", "Y", 0, l => l.Y), ("width", "W", 1, l => l.Width), ("height", "H", 1, l => l.Height) };
        for (var i = 0; i < boxes.Length; i++) {
            var (property, label, min, read) = boxes[i];
            position.ColumnDefinitions.Add(new() { Width = new GridLength(1, GridUnitType.Star) });
            var box = Number(label, min, 8192, read(layer), value => SetNumbers(id, new Dictionary<string, double> { [property] = value }));
            AutomationProperties.SetName(box, "Layer " + label);
            if (property == "width" && IsAutoWidth(content)) {
                _inspectorInputs.Remove(box);
                box.IsEnabled = false;
            }
            _inspectorRefresh.Add(l => Show(box, read(l)));
            Grid.SetColumn(box, i); position.Children.Add(box);
        }
        _layerBody.Children.Add(position);
        var opacity = new SliderValueControl { Header = "Opacity", Minimum = 0, Maximum = 1, StepFrequency = .01, Unit = "%", DisplayScale = 100, DefaultValue = 1, Value = layer.Opacity };
        opacity.ValueChanged += (_, value) => { if (!_loading) SetNumbers(id, new Dictionary<string, double> { ["opacity"] = value }); };
        opacity.EditStarted += (_, _) => _session?.BeginGesture(); opacity.EditCompleted += (_, _) => _session?.EndGesture();
        _inspectorRefresh.Add(l => { if (Math.Abs(opacity.Value - l.Opacity) > 1e-9) opacity.Value = l.Opacity; });
        _inspectorInputs.Add(opacity); _layerBody.Children.Add(opacity);
        AddContentControls(layer, content);
    }

    private void BuildCrawl(GraphicsLayerInspection layer, JsonElement node)
    {
        var direction = Choice("Direction", [("left", "Left"), ("right", "Right")], ReadString(node, "direction", "left"));
        var mode = Choice("Playback", [("loop", "Loop"), ("single", "Single pass")], ReadString(node, "mode", "loop"));
        void SelectOption(ComboBox box, string property)
        {
            if (!_loading && box.SelectedValue is string value)
                AfterSelectionChanged(() => Apply(s => GraphicsTemplateEditor.Set(s, layer.Id, property, JsonSerializer.SerializeToElement(value))));
        }
        direction.SelectionChanged += (_, _) => SelectOption(direction, "direction");
        mode.SelectionChanged += (_, _) => SelectOption(mode, "mode");
        var speed = Number("Speed (px/s)", 1, 1200, ReadNumber(node, "speed", 120),
            value => AfterSelectionChanged(() => SetNumbers(layer.Id, new Dictionary<string, double> { ["speed"] = value })));
        var gap = Number("Gap (px)", 0, 512, ReadNumber(node, "gap", 48),
            value => AfterSelectionChanged(() => SetNumbers(layer.Id, new Dictionary<string, double> { ["gap"] = value })));
        _layerBody.Children.Add(Columns([direction, mode], [1, 1]));
        _layerBody.Children.Add(Columns([speed, gap], [1, 1]));
        var font = Choice("Typeface", GraphicsDesignerModel.FontChoices(layer.FontChoice), layer.FontChoice);
        font.SelectionChanged += (_, _) =>
        {
            if (!_loading && font.SelectedValue is string value)
                AfterSelectionChanged(() => Apply(s => GraphicsDesignerModel.SetTypeface(s, layer.Id, value)));
        };
        _layerBody.Children.Add(font);
        _layerBody.Children.Add(Number("Font size", 1, Math.Clamp(layer.Height, 1, 512), layer.FontSize,
            value => SetNumbers(layer.Id, new Dictionary<string, double> { ["fontSize"] = value })));
        _layerBody.Children.Add(ColourChoice("Colour", "color", layer.ColourChoice, layer.Id));
    }

    private void BuildText(GraphicsLayerInspection layer, IReadOnlyList<NativeGraphicsTemplateField> fields)
    {
        var id = layer.Id;
        var textFields = fields.Where(f => f.Type == "string").Select(f => f.Id).ToList();
        var shows = Choice("Shows", [("", "Fixed text"), .. textFields.Select(f => (f, "Variable: " + f))], layer.ShowsVariable ?? "");
        var fixedText = new TextBox { Header = "Fixed text", Text = layer.FixedText, MaxLength = 512, FontSize = 12, TextWrapping = TextWrapping.Wrap, Visibility = layer.ShowsVariable is null ? Visibility.Visible : Visibility.Collapsed };
        AutomationProperties.SetName(fixedText, "Fixed text"); _inspectorInputs.Add(fixedText);
        shows.SelectionChanged += (_, _) => {
            if (_loading || shows.SelectedValue is not string chosen) return;
            var variable = chosen.Length == 0 ? null : chosen;
            var text = fixedText.Text.Length > 0 ? fixedText.Text : "Text";
            AfterSelectionChanged(() => Apply(s => GraphicsDesignerModel.SetShows(s, id, variable, text)));
        };
        // Checked and written against the captured layer id, never the current selection: the commit may run after the
        // selection moved (flushed by SelectLayer / RefreshAll, or a late LostFocus from the removed box).
        bool CommitFixed()
        {
            if (_loading || _session?.TemplateDocument is not { } document || _session.Draft is not { } draft) return false;
            GraphicsLayerInspection own;
            try { own = GraphicsDesignerModel.Inspect(document, draft.Fields, id); }
            catch (Exception ex) when (ex is InvalidDataException or JsonException) { return false; }
            var text = fixedText.Text;
            return own is { ShowsVariable: null, Locked: false } && text != own.FixedText && Apply(s => GraphicsDesignerModel.SetShows(s, id, null, text));
        }
        fixedText.TextChanged += (_, _) => { if (!_loading && fixedText.FocusState != FocusState.Unfocused) _pending.Track(id, CommitFixed); };
        _inspectorRefresh.Add(l => { if (fixedText.FocusState == FocusState.Unfocused && fixedText.Text != l.FixedText) fixedText.Text = l.FixedText; });
        fixedText.KeyDown += (_, e) => { if (e.Key == Windows.System.VirtualKey.Enter) { e.Handled = true; _pending.Clear(); CommitFixed(); } };
        fixedText.LostFocus += (_, _) => { if (_pending.Owner == id) _pending.Flush(); else CommitFixed(); };
        _layerBody.Children.Add(shows); _layerBody.Children.Add(fixedText);

        var typeface = Choice("Typeface", GraphicsDesignerModel.FontChoices(layer.FontChoice), layer.FontChoice);
        typeface.SelectionChanged += (_, _) => { if (!_loading && typeface.SelectedValue is string font && font != CurrentInspection()?.FontChoice) AfterSelectionChanged(() => Apply(s => GraphicsDesignerModel.SetTypeface(s, id, font))); };
        var size = Number("Size", 1, 512, layer.FontSize, value => SetNumbers(id, new Dictionary<string, double> { ["fontSize"] = value }));
        _inspectorRefresh.Add(l => Show(size, l.FontSize));
        _layerBody.Children.Add(Columns([typeface, size], [2, 1]));

        var weight = Choice("Weight", GraphicsDesignerChoices.Weights.Select(w => (w.Id.ToString(System.Globalization.CultureInfo.InvariantCulture), w.Label)).ToList(), layer.FontWeight.ToString(System.Globalization.CultureInfo.InvariantCulture));
        weight.SelectionChanged += (_, _) => { if (!_loading && weight.SelectedValue is string w && int.TryParse(w, out var value) && value != CurrentInspection()?.FontWeight) SetNumbers(id, new Dictionary<string, double> { ["fontWeight"] = value }); };
        _inspectorRefresh.Add(l => Select(weight, l.FontWeight.ToString(System.Globalization.CultureInfo.InvariantCulture)));
        var colour = ColourChoice("Colour", "color", layer.ColourChoice, id);
        var lines = Choice("Lines", GraphicsDesignerChoices.Lines.Select(n => (n.ToString(System.Globalization.CultureInfo.InvariantCulture), n.ToString(System.Globalization.CultureInfo.InvariantCulture))).ToList(), layer.MaxLines.ToString(System.Globalization.CultureInfo.InvariantCulture));
        lines.SelectionChanged += (_, _) => { if (!_loading && lines.SelectedValue is string n && int.TryParse(n, out var value) && value != CurrentInspection()?.MaxLines) SetNumbers(id, new Dictionary<string, double> { ["maxLines"] = value }); };
        _inspectorRefresh.Add(l => Select(lines, l.MaxLines.ToString(System.Globalization.CultureInfo.InvariantCulture)));
        _layerBody.Children.Add(colour);
        _layerBody.Children.Add(Columns([weight, lines], [1, 1]));

        var overflow = LiteralChoice("Overflow", "overflow", GraphicsDesignerChoices.Overflows, layer.Overflow, id, l => l.Overflow);
        var align = LiteralChoice("Align", "align", GraphicsDesignerChoices.Aligns, layer.Align, id, l => l.Align);
        _layerBody.Children.Add(Columns([overflow, align], [1, 1]));
    }

    private void BuildShape(GraphicsLayerInspection layer)
    {
        var fill = ColourChoice("Fill", "fill", layer.FillChoice, layer.Id);
        if (layer.FillGradient) ToolTipService.SetToolTip(fill, "This fill is a gradient. Choosing a colour replaces it.");
        _layerBody.Children.Add(fill);
        if (layer.FillGradient) _layerBody.Children.Add(Note("Gradient fill. Choose a colour to replace it."));
        if (layer.Kind == "Shape") {
            var radius = Number("Corner radius", 0, 4096, layer.CornerRadius, value => SetNumbers(layer.Id, new Dictionary<string, double> { ["cornerRadius"] = value }));
            _inspectorRefresh.Add(l => Show(radius, l.CornerRadius));
            _layerBody.Children.Add(radius);
        }
    }

    private void BuildImage(GraphicsLayerInspection layer, IReadOnlyList<NativeGraphicsTemplateField> fields, NativeGraphicsScratch? document)
    {
        var id = layer.Id;
        var sources = new List<(string, string)>();
        if (document is not null) {
            try { sources.AddRange(GraphicsDesignerModel.ImageSources(document)); }
            catch (Exception ex) when (ex is InvalidDataException or JsonException) { Say(ex.Message); }
        }
        if (layer.ImageSource.Length > 0 && sources.All(s => s.Item1 != layer.ImageSource)) sources.Add((layer.ImageSource, layer.ImageSource.StartsWith("token:", StringComparison.Ordinal) ? "Brand logo" : layer.ImageSource));
        var source = Choice("Source", sources, layer.ImageSource);
        source.SelectionChanged += (_, _) => {
            if (_loading || source.SelectedValue is not string chosen || chosen == CurrentInspection()?.ImageSource) return;
            if (chosen.StartsWith("field:", StringComparison.Ordinal)) AfterSelectionChanged(() => Apply(s => GraphicsTemplateEditor.BindLayer(s, id, "asset", chosen["field:".Length..], null)));
            else if (chosen.StartsWith("asset:", StringComparison.Ordinal)) AfterSelectionChanged(() => Apply(s => GraphicsTemplateEditor.BindLayer(s, id, "asset", null, JsonSerializer.SerializeToElement(chosen["asset:".Length..]))));
        };
        _layerBody.Children.Add(source);
        var fit = LiteralChoice("Fit", "fit", GraphicsDesignerChoices.Fits, layer.Fit, id, l => l.Fit);
        _layerBody.Children.Add(fit);
        var align = LiteralChoice("Align", "align", GraphicsDesignerChoices.Aligns, layer.ImageAlign, id, l => l.ImageAlign);
        var vertical = LiteralChoice("Vertical align", "verticalAlign", GraphicsDesignerChoices.VerticalAligns, layer.ImageVerticalAlign, id, l => l.ImageVerticalAlign);
        _layerBody.Children.Add(Columns([align, vertical], [1, 1]));
    }

    // ---- inspector inputs ---------------------------------------------------------------------
    private GraphicsLayerInspection? CurrentInspection()
    {
        if (_session?.TemplateDocument is not { } document || _session.Draft is not { } draft || _session.SelectedLayerId.Length == 0) return null;
        try { return GraphicsDesignerModel.Inspect(document, draft.Fields, _session.SelectedLayerId); }
        catch (Exception ex) when (ex is InvalidDataException or JsonException) { return null; }
    }

    private ComboBox Choice(string header, IReadOnlyList<(string Id, string Label)> choices, string selected)
    {
        var box = new ComboBox { Header = header, FontSize = 12, HorizontalAlignment = HorizontalAlignment.Stretch, SelectedValuePath = "Tag" };
        foreach (var (id, label) in choices) box.Items.Add(new ComboBoxItem { Content = label, Tag = id });
        box.SelectedValue = selected;
        AutomationProperties.SetName(box, header); _inspectorInputs.Add(box);
        return box;
    }
    private void Select(ComboBox box, string value) { if (!Equals(box.SelectedValue, value)) box.SelectedValue = value; }
    /// <summary>An edit chosen in a ComboBox that changes the inspector's shape (Shows, Typeface, Colour, Fill, image
    /// Source) runs after the box's own SelectionChanged / TextSubmitted returns: the edit rebuilds the inspector, and
    /// removing a ComboBox inside its own event is not safe in WinUI.</summary>
    private void AfterSelectionChanged(Action edit) { if (DispatcherQueue?.TryEnqueue(() => edit()) != true) edit(); }

    private ComboBox LiteralChoice(string header, string property, IReadOnlyList<(string Id, string Label)> choices, string selected, string id, Func<GraphicsLayerInspection, string> read)
    {
        var box = Choice(header, choices, selected);
        box.SelectionChanged += (_, _) => {
            if (_loading || box.SelectedValue is not string value || CurrentInspection() is not { } now || read(now) == value) return;
            Apply(s => GraphicsTemplateEditor.BindLayer(s, id, property, null, JsonSerializer.SerializeToElement(value)));
        };
        _inspectorRefresh.Add(l => Select(box, read(l)));
        return box;
    }

    /// <summary>Brand colours plus the layer's own #hex (appended); type a #RRGGBB to use another colour.</summary>
    private ComboBox ColourChoice(string header, string property, string current, string id)
    {
        IReadOnlyList<(string Id, string Label)> choices = current.Length > 0 && GraphicsDesignerChoices.Colours.All(c => c.Id != current)
            ? [.. GraphicsDesignerChoices.Colours, (current, current)] : GraphicsDesignerChoices.Colours;
        var box = Choice(header, choices, current);
        box.IsEditable = true; box.PlaceholderText = current.Length == 0 ? "Follows a variable" : "#RRGGBB";
        ToolTipService.SetToolTip(box, "Choose a brand colour or type a colour like #FF8800.");
        void Reset() { var label = choices.FirstOrDefault(c => c.Id == current).Label; box.Text = label ?? ""; }
        box.SelectionChanged += (_, _) => {
            if (_loading || box.SelectedValue is not string value || value == current) return;
            AfterSelectionChanged(() => { if (!Apply(s => GraphicsDesignerModel.SetColour(s, id, property, value))) Reset(); });
        };
        box.TextSubmitted += (_, e) => {
            var typed = e.Text.Trim(); var match = choices.FirstOrDefault(c => c.Label == typed || c.Id == typed);
            e.Handled = true;
            if (match.Id is { } known) { if (known != current) AfterSelectionChanged(() => { if (!Apply(s => GraphicsDesignerModel.SetColour(s, id, property, known))) Reset(); }); return; }
            // A refusal ("orange") leaves the layer's colour; show that colour again rather than the rejected text.
            AfterSelectionChanged(() => { if (!Apply(s => GraphicsDesignerModel.SetColour(s, id, property, typed))) Reset(); });
        };
        return box;
    }

    private NumberBox Number(string header, double min, double max, double value, Action<double> commit)
    {
        var box = new NumberBox { Header = header, Minimum = min, Maximum = max, Value = value, FontSize = 12, SpinButtonPlacementMode = NumberBoxSpinButtonPlacementMode.Hidden,
            ValidationMode = NumberBoxValidationMode.InvalidInputOverwritten, HorizontalAlignment = HorizontalAlignment.Stretch,
            NumberFormatter = new Windows.Globalization.NumberFormatting.DecimalFormatter { FractionDigits = 0, IsDecimalPointAlwaysDisplayed = false,
                NumberRounder = new Windows.Globalization.NumberFormatting.IncrementNumberRounder { Increment = 0.01 } } };
        AutomationProperties.SetName(box, header); _inspectorInputs.Add(box);
        box.ValueChanged += (_, e) => { if (!_loading && !double.IsNaN(e.NewValue) && Math.Abs(e.NewValue - e.OldValue) > 1e-9) commit(e.NewValue); };
        return box;
    }
    private void Show(NumberBox box, double value) { if (box.FocusState == FocusState.Unfocused && Math.Abs(box.Value - value) > 1e-9) box.Value = value; }

    private static Grid Columns(FrameworkElement[] children, double[] weights)
    {
        var grid = new Grid { ColumnSpacing = 8 };
        for (var i = 0; i < children.Length; i++) {
            grid.ColumnDefinitions.Add(new() { Width = new GridLength(weights[i], GridUnitType.Star) });
            Grid.SetColumn(children[i], i); grid.Children.Add(children[i]);
        }
        return grid;
    }
    private static TextBlock Note(string text) => new() { Text = text, FontSize = 12, Foreground = StudioBrushes.Muted, TextWrapping = TextWrapping.Wrap };
}
