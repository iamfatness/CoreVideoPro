using System.Text.Json;
using CoreVideoPro.MediaCore.Models;
using CoreVideoPro.MediaCore.Services;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Automation;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Media;

namespace CoreVideoPro.WinUI.Controls;

/// <summary>Graphics Designer Variables panel (spec §4, mockup D3): the open design's variables with
/// add / rename / delete, kind, max length, sample value, "If empty" and "Used by". Every change edits
/// the workspace session's PRIVATE draft (template scratch + matching draft values) through
/// <see cref="GraphicsTemplateEditor"/>; refusals are shown in the panel's status line.</summary>
public sealed class GraphicsVariablesPanel : UserControl
{
    private static readonly (string Kind, string Label)[] KindChoices = [("text", "Text"), ("number", "Number"), ("colour", "Colour"), ("on-off", "On / off"), ("image", "Image")];
    private const int MaxVariables = 32;

    private readonly StackPanel _rows = new() { Spacing = 6 };
    private readonly TextBlock _count = Body(""), _status = Body(""), _empty = Body("Open a design to edit its variables.");
    private readonly StackPanel _content = new() { Spacing = 10 };
    private readonly StackPanel _editor = new() { Spacing = 8, Padding = new Thickness(10), BorderThickness = new Thickness(1), CornerRadius = new CornerRadius(8) };
    private readonly TextBlock _editorTitle = new() { FontSize = 15, FontWeight = Microsoft.UI.Text.FontWeights.SemiBold, TextWrapping = TextWrapping.Wrap };
    private readonly TextBlock _usedBy = Body("");
    private readonly TextBox _name = new() { Header = "Name", MaxLength = 64, FontSize = 14 };
    private readonly NumberBox _maxLength = new() { Header = "Max length (characters)", Minimum = 0, Maximum = 4096, SpinButtonPlacementMode = NumberBoxSpinButtonPlacementMode.Compact, FontSize = 14 };
    private readonly TextBox _sampleText = new() { Header = "Sample value", MaxLength = 4096, FontSize = 14 };
    private readonly NumberBox _sampleNumber = new() { Header = "Sample value", SpinButtonPlacementMode = NumberBoxSpinButtonPlacementMode.Compact, FontSize = 14 };
    private readonly ToggleSwitch _sampleOn = new() { Header = "Sample value", OnContent = "On", OffContent = "Off" };
    private readonly ComboBox _ifEmpty = new() { Header = "If empty", ItemsSource = new[] { "Hide its layer", "Show the sample" }, HorizontalAlignment = HorizontalAlignment.Stretch, FontSize = 14 };
    private readonly Button _rename = StudioBrushes.GhostButton("Rename"), _apply = StudioBrushes.GhostButton("Apply"), _delete = StudioBrushes.GhostButton("Delete variable");
    private readonly TextBox _newName = new() { Header = "New variable", MaxLength = 64, PlaceholderText = "location", FontSize = 14 };
    private readonly ComboBox _newKind = new() { Header = "Kind", ItemsSource = KindChoices.Select(k => k.Label).ToArray(), SelectedIndex = 0, HorizontalAlignment = HorizontalAlignment.Stretch, FontSize = 14 };
    private readonly NumberBox _newMaxLength = new() { Header = "Max length", Minimum = 0, Maximum = 4096, Value = 128, SpinButtonPlacementMode = NumberBoxSpinButtonPlacementMode.Compact, FontSize = 14 };
    private readonly Button _add = StudioBrushes.AccentButton("Add variable");

    private GraphicsWorkspaceSession? _session;
    private IReadOnlyList<GraphicsEditorVariable> _variables = [];
    private string _selected = "", _signature = "";

    public GraphicsVariablesPanel()
    {
        var root = new StackPanel { Spacing = 10 };
        _count.Foreground = StudioBrushes.Muted;
        root.Children.Add(_count); root.Children.Add(_empty);
        _content.Children.Add(Body("The words and values that change each time this graphic is used. Layers show a variable or fixed text."));
        _content.Children.Add(_rows);

        _editor.Background = StudioBrushes.SurfaceRaised; _editor.BorderBrush = StudioBrushes.Border;
        _editorTitle.Foreground = StudioBrushes.Text;
        _editor.Children.Add(_editorTitle); _editor.Children.Add(_usedBy);
        var renameRow = new Grid { ColumnSpacing = 8 }; renameRow.ColumnDefinitions.Add(new() { Width = new GridLength(1, GridUnitType.Star) }); renameRow.ColumnDefinitions.Add(new() { Width = GridLength.Auto });
        renameRow.Children.Add(_name); _rename.VerticalAlignment = VerticalAlignment.Bottom; Grid.SetColumn(_rename, 1); renameRow.Children.Add(_rename);
        _editor.Children.Add(renameRow);
        foreach (var control in new FrameworkElement[] { _maxLength, _sampleText, _sampleNumber, _sampleOn, _ifEmpty }) { control.HorizontalAlignment = HorizontalAlignment.Stretch; _editor.Children.Add(control); }
        var editActions = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 8 }; editActions.Children.Add(_apply); editActions.Children.Add(_delete);
        _editor.Children.Add(editActions);
        _content.Children.Add(_editor);

        var addRow = new Grid { ColumnSpacing = 8, RowSpacing = 6 };
        addRow.ColumnDefinitions.Add(new() { Width = new GridLength(3, GridUnitType.Star) }); addRow.ColumnDefinitions.Add(new() { Width = new GridLength(2, GridUnitType.Star) });
        addRow.RowDefinitions.Add(new() { Height = GridLength.Auto }); addRow.RowDefinitions.Add(new() { Height = GridLength.Auto });
        addRow.Children.Add(_newName); Grid.SetColumn(_newKind, 1); addRow.Children.Add(_newKind);
        Grid.SetRow(_newMaxLength, 1); addRow.Children.Add(_newMaxLength); _add.VerticalAlignment = VerticalAlignment.Bottom; Grid.SetRow(_add, 1); Grid.SetColumn(_add, 1); addRow.Children.Add(_add);
        _content.Children.Add(addRow);
        _status.TextWrapping = TextWrapping.Wrap; _content.Children.Add(_status);
        root.Children.Add(_content);
        Content = root;

        AutomationProperties.SetName(_name, "Variable name"); AutomationProperties.SetName(_maxLength, "Variable max length");
        AutomationProperties.SetName(_ifEmpty, "If the variable is empty"); AutomationProperties.SetName(_newName, "New variable name");
        AutomationProperties.SetName(_newKind, "New variable kind"); AutomationProperties.SetName(_newMaxLength, "New variable max length");
        AutomationProperties.SetName(_rename, "Rename variable"); AutomationProperties.SetName(_apply, "Apply variable options");
        AutomationProperties.SetName(_delete, "Delete variable"); AutomationProperties.SetName(_add, "Add variable");
        ToolTipService.SetToolTip(_ifEmpty, "Hide its layer: the line disappears when the value is blank. Show the sample: the sample stays visible.");

        _newKind.SelectionChanged += (_, _) => _newMaxLength.Visibility = _newKind.SelectedIndex == 0 ? Visibility.Visible : Visibility.Collapsed;
        _add.Click += (_, _) => AddVariable();
        _rename.Click += (_, _) => RenameVariable();
        _apply.Click += (_, _) => ApplyOptions();
        _delete.Click += (_, _) => DeleteVariable();
        Refresh();
    }

    public GraphicsWorkspaceSession? Session { get => _session; set { _session = value; _signature = ""; Refresh(); } }

    /// <summary>Rebuilds rows and the editor only when the edited manifest or the selection changed,
    /// so typing in the editor is never overwritten by a model refresh.</summary>
    public void Refresh()
    {
        var document = _session?.Draft is { Declarative: true } ? _session.TemplateDocument : null;
        _empty.Visibility = document is null ? Visibility.Visible : Visibility.Collapsed;
        _content.Visibility = document is null ? Visibility.Collapsed : Visibility.Visible;
        _add.IsEnabled = _rename.IsEnabled = _apply.IsEnabled = _delete.IsEnabled = _session is { Busy: false };
        if (document is null) { _signature = ""; return; }
        var signature = document.Hash + "|" + _selected;
        if (signature == _signature) return;
        try { _variables = GraphicsTemplateEditor.Variables(document); }
        catch (Exception ex) when (ex is InvalidDataException or InvalidOperationException or JsonException) { _variables = []; _status.Text = ex.Message; }
        if (_variables.All(v => v.Id != _selected)) _selected = _variables.FirstOrDefault()?.Id ?? "";
        _signature = document.Hash + "|" + _selected;
        _count.Text = $"{_variables.Count} of {MaxVariables} variables";
        _rows.Children.Clear();
        foreach (var variable in _variables) _rows.Children.Add(Row(variable));
        ShowEditor(_variables.FirstOrDefault(v => v.Id == _selected));
    }

    private Button Row(GraphicsEditorVariable variable)
    {
        var selected = variable.Id == _selected;
        var body = new StackPanel { Spacing = 2 };
        var top = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 8 };
        top.Children.Add(new TextBlock { Text = variable.Id, FontSize = 14, FontWeight = Microsoft.UI.Text.FontWeights.SemiBold, Foreground = StudioBrushes.Text });
        top.Children.Add(new TextBlock { Text = KindLabel(variable) + (variable.Required ? " · required" : ""), FontSize = 14, Foreground = StudioBrushes.Muted });
        body.Children.Add(top);
        body.Children.Add(new TextBlock { Text = UsedBy(variable), FontSize = 14, Foreground = StudioBrushes.Muted, TextWrapping = TextWrapping.Wrap });
        var row = new Button { Content = body, HorizontalAlignment = HorizontalAlignment.Stretch, HorizontalContentAlignment = HorizontalAlignment.Left, Padding = new Thickness(10, 6, 10, 6), CornerRadius = new CornerRadius(6),
            Background = selected ? StudioBrushes.SurfaceRaised : StudioBrushes.Panel, BorderBrush = selected ? StudioBrushes.Accent : StudioBrushes.Border, BorderThickness = new Thickness(selected ? 2 : 1) };
        AutomationProperties.SetName(row, "Variable " + variable.Id);
        row.Click += (_, _) => { _selected = variable.Id; _status.Text = ""; Refresh(); };
        return row;
    }

    private void ShowEditor(GraphicsEditorVariable? variable)
    {
        _editor.Visibility = variable is null ? Visibility.Collapsed : Visibility.Visible;
        if (variable is null) return;
        _editorTitle.Text = $"{variable.Id} · {KindLabel(variable)}";
        _usedBy.Text = UsedBy(variable);
        _name.Text = variable.Id;
        var text = variable.Kind == "text";
        _maxLength.Visibility = text ? Visibility.Visible : Visibility.Collapsed; _maxLength.Value = variable.MaxLength ?? double.NaN;
        _sampleText.Visibility = variable.Kind is "text" or "colour" or "image" ? Visibility.Visible : Visibility.Collapsed;
        _sampleText.PlaceholderText = variable.Kind == "colour" ? "#RRGGBB" : variable.Kind == "image" ? "Image asset id (empty for none)" : "";
        _sampleText.Text = variable.Sample.ValueKind == JsonValueKind.String ? variable.Sample.GetString() ?? "" : "";
        _sampleNumber.Visibility = variable.Kind == "number" ? Visibility.Visible : Visibility.Collapsed;
        _sampleNumber.Value = variable.Sample.ValueKind == JsonValueKind.Number ? variable.Sample.GetDouble() : 0;
        _sampleOn.Visibility = variable.Kind == "on-off" ? Visibility.Visible : Visibility.Collapsed;
        _sampleOn.IsOn = variable.Sample.ValueKind == JsonValueKind.True;
        _ifEmpty.Visibility = variable.Kind is "text" or "image" ? Visibility.Visible : Visibility.Collapsed;
        _ifEmpty.SelectedIndex = variable.HideWhenEmpty ? 0 : 1;
        _delete.IsEnabled = _session is { Busy: false };
    }

    private void AddVariable()
    {
        var id = _newName.Text.Trim();
        var kind = KindChoices[Math.Max(0, _newKind.SelectedIndex)].Kind;
        int? maxLength = kind == "text" && !double.IsNaN(_newMaxLength.Value) ? (int)Math.Round(_newMaxLength.Value) : null;
        if (Apply(s => GraphicsTemplateEditor.AddField(s, id, kind, maxLength), $"Added {id}. Bind a layer to it in Edit layers, then Save design.")) {
            _selected = id; _newName.Text = ""; Refresh();
        }
    }

    private void RenameVariable()
    {
        var from = _selected; var to = _name.Text.Trim();
        if (from.Length == 0 || from == to) return;
        if (Apply(s => GraphicsTemplateEditor.RenameField(s, from, to), $"Renamed {from} to {to}; every layer that used it now uses {to}.", from, to)) { _selected = to; Refresh(); }
    }

    private void ApplyOptions()
    {
        if (_variables.FirstOrDefault(v => v.Id == _selected) is not { } variable) return;
        int? maxLength = variable.Kind == "text" ? (double.IsNaN(_maxLength.Value) ? null : (int)Math.Round(_maxLength.Value)) : variable.MaxLength;
        JsonElement sample = variable.Kind switch {
            "number" => JsonSerializer.SerializeToElement(double.IsNaN(_sampleNumber.Value) ? 0 : _sampleNumber.Value),
            "on-off" => JsonSerializer.SerializeToElement(_sampleOn.IsOn),
            _ => JsonSerializer.SerializeToElement(_sampleText.Text),
        };
        var hide = variable.Kind is "text" or "image" ? _ifEmpty.SelectedIndex == 0 : variable.HideWhenEmpty;
        Apply(s => GraphicsTemplateEditor.SetFieldOptions(s, variable.Id, maxLength, sample, hide), $"Updated {variable.Id}.");
    }

    private void DeleteVariable()
    {
        var id = _selected; if (id.Length == 0) return;
        if (Apply(s => GraphicsTemplateEditor.RemoveField(s, id), $"Deleted {id}.", failurePrefix: $"{id} is still on a layer, so it was kept. ")) { _selected = ""; Refresh(); }
    }

    /// <summary>One private edit: the template change plus draft values matched to the new variables
    /// (native refuses unknown or missing fields). A refusal leaves the draft untouched and says why.</summary>
    private bool Apply(Func<NativeGraphicsScratch, NativeGraphicsScratch> change, string done, string? renamedFrom = null, string? renamedTo = null, string failurePrefix = "")
    {
        if (_session is not { } session) return false;
        if (session.Busy) { _status.Text = "Graphics is busy. Try again in a moment."; return false; }
        if (session.Draft is not { Declarative: true } draft || session.TemplateDocument is not { } document) { _status.Text = "Open a design to edit its variables."; return false; }
        NativeGraphicsScratch edited; JsonElement fields;
        try { edited = change(document); fields = GraphicsTemplateEditor.SyncFields(draft.Fields, edited, renamedFrom, renamedTo); }
        catch (Exception ex) when (ex is InvalidDataException or InvalidOperationException or JsonException or ArgumentException) {
            _status.Text = failurePrefix + ex.Message; return false;
        }
        session.Edit(draft with { Scratch = edited.Hash == session.Template?.ManifestHash ? null : edited, Fields = fields });
        _status.Text = done + " Private draft only; Save design keeps it.";
        return true;
    }

    private static string KindLabel(GraphicsEditorVariable variable) =>
        (KindChoices.FirstOrDefault(k => k.Kind == variable.Kind).Label ?? "Text") + (variable.Kind == "text" && variable.MaxLength is int max ? $" (max {max})" : "");
    private static string UsedBy(GraphicsEditorVariable variable) => variable.UsedBy.Count == 0 ? "Not on any layer yet" : "Used by: " + string.Join(", ", variable.UsedBy);
    private static TextBlock Body(string text) => new() { Text = text, FontSize = 14, TextWrapping = TextWrapping.Wrap, Foreground = StudioBrushes.Text };
}
