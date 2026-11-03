using CoreVideoPro.MediaCore.Models;
using CoreVideoPro.MediaCore.Services;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Automation;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Controls.Primitives;
using Microsoft.UI.Xaml.Input;
using Microsoft.UI.Xaml.Media;
using Windows.ApplicationModel.DataTransfer;
using Windows.System;

namespace CoreVideoPro.WinUI.Controls;

/// <summary>The Designer's layers panel: rows front-to-back (<see cref="GraphicsDesignerModel.Rows"/>), eye / lock toggles,
/// double-click rename, drag to reorder or into a group (<see cref="GraphicsDesignerModel.DropTarget"/>), the structure
/// toolbar and the add buttons.</summary>
public sealed partial class GraphicsDesignerControl
{
    private static readonly FontFamily Icons = new("Segoe MDL2 Assets");
    private const string ClockRefusal = "Clocks tick in clock and countdown designs. Start one from the Clock look.";
    private readonly StackPanel _rowsPanel = new() { Spacing = 3 };
    private readonly Border _endDrop = new() { MinHeight = 28, Background = new SolidColorBrush(Microsoft.UI.Colors.Transparent), AllowDrop = true };
    private readonly Button _duplicate = SmallButton("Duplicate"), _delete = SmallButton("Delete"), _group = SmallButton("Group"), _ungroup = SmallButton("Ungroup");
    private readonly Button _addText = AddButton("Text"), _addShape = AddButton("Shape"), _addImage = AddButton("Image…"), _addClock = AddButton("Clock");
    private readonly Border _clockHost = new();
    private readonly MenuFlyout _imageSources = new() { Placement = FlyoutPlacementMode.Bottom };
    private IReadOnlyList<GraphicsDesignerRow> _rows = [];
    private string _rowsSignature = "", _renaming = "", _dragId = "";

    private static Button SmallButton(string label) { var b = StudioBrushes.GhostButton(label); b.FontSize = 13; b.FontWeight = Microsoft.UI.Text.FontWeights.Normal; b.Padding = new Thickness(8, 4, 8, 4); b.MinHeight = 30; return b; }
    private static Button AddButton(string label)
    {
        var b = StudioBrushes.GhostButton(label); b.FontSize = 14; b.FontWeight = Microsoft.UI.Text.FontWeights.Normal; b.Background = StudioBrushes.SurfaceRaised;
        b.HorizontalAlignment = HorizontalAlignment.Stretch; b.MinHeight = 34; return b;
    }

    private Border BuildLayersPanel()
    {
        var grid = new Grid { RowSpacing = 8 };
        foreach (var height in new[] { GridLength.Auto, GridLength.Auto, new GridLength(1, GridUnitType.Star), GridLength.Auto, GridLength.Auto }) grid.RowDefinitions.Add(new() { Height = height });
        grid.Children.Add(Heading("LAYERS"));
        var toolbar = new Grid { ColumnSpacing = 4, RowSpacing = 4 };
        toolbar.ColumnDefinitions.Add(new() { Width = new GridLength(1, GridUnitType.Star) }); toolbar.ColumnDefinitions.Add(new() { Width = new GridLength(1, GridUnitType.Star) });
        toolbar.RowDefinitions.Add(new() { Height = GridLength.Auto }); toolbar.RowDefinitions.Add(new() { Height = GridLength.Auto });
        var tools = new[] { (_duplicate, "Duplicate layer", "Copy the selected layer just in front of it."), (_delete, "Delete layer", "Remove the selected layer."),
            (_group, "Group layer", "Wrap the selected layer in a new group."), (_ungroup, "Ungroup", "Replace the selected group with its layers.") };
        for (var i = 0; i < tools.Length; i++) {
            var (button, name, tip) = tools[i]; button.HorizontalAlignment = HorizontalAlignment.Stretch;
            AutomationProperties.SetName(button, name); ToolTipService.SetToolTip(button, tip);
            Grid.SetRow(button, i / 2); Grid.SetColumn(button, i % 2); toolbar.Children.Add(button);
        }
        _duplicate.Click += (_, _) => { if (Selected() is { } row) { Apply(s => { var copy = GraphicsTemplateEditor.Duplicate(s, row.Id, out var id); _pendingSelect = id; return copy; }); SelectPending(); } };
        _delete.Click += (_, _) => { if (Selected() is { } row) Apply(s => GraphicsTemplateEditor.Remove(s, row.Id), done: $"Deleted {row.Label}. Undo brings it back."); };
        _group.Click += (_, _) => { if (Selected() is { } row) { Apply(s => { var (grouped, id) = GraphicsTemplateEditor.Group(s, row.Id); _pendingSelect = id; return grouped; }); SelectPending(); } };
        _ungroup.Click += (_, _) => { if (Selected() is { Kind: "Group" } row) Apply(s => GraphicsTemplateEditor.Ungroup(s, row.Id)); };
        Grid.SetRow(toolbar, 1); grid.Children.Add(toolbar);

        var list = new StackPanel(); list.Children.Add(_rowsPanel); list.Children.Add(_endDrop);
        AutomationProperties.SetName(_rowsPanel, "Layers");
        _endDrop.DragOver += (_, e) => { e.AcceptedOperation = DropAt(_rows.Count, false) is null ? DataPackageOperation.None : DataPackageOperation.Move; };
        _endDrop.Drop += (_, e) => DropLayer(_rows.Count, false);
        var scroll = new ScrollViewer { Content = list, VerticalScrollBarVisibility = ScrollBarVisibility.Auto, HorizontalScrollBarVisibility = ScrollBarVisibility.Disabled };
        Grid.SetRow(scroll, 2); grid.Children.Add(scroll);
        var hint = new TextBlock { Text = "Top of the list is in front. Drag to reorder, double-click to rename.", FontSize = 13, Foreground = StudioBrushes.Muted, TextWrapping = TextWrapping.Wrap };
        Grid.SetRow(hint, 3); grid.Children.Add(hint);

        var add = new Grid { ColumnSpacing = 6, RowSpacing = 6 };
        add.ColumnDefinitions.Add(new() { Width = new GridLength(1, GridUnitType.Star) }); add.ColumnDefinitions.Add(new() { Width = new GridLength(1, GridUnitType.Star) });
        add.RowDefinitions.Add(new() { Height = GridLength.Auto }); add.RowDefinitions.Add(new() { Height = GridLength.Auto });
        AutomationProperties.SetName(_addText, "Add text layer"); AutomationProperties.SetName(_addShape, "Add shape layer");
        AutomationProperties.SetName(_addImage, "Add image layer"); AutomationProperties.SetName(_addClock, "Add clock layer");
        _addText.Click += (_, _) => AddLayer("text"); _addClock.Click += (_, _) => AddLayer("clock");
        var shapes = new MenuFlyout();
        foreach (var (kind, label) in new[] { ("shape", "Rectangle"), ("ellipse", "Ellipse"), ("line", "Line") }) {
            var item = new MenuFlyoutItem { Text = label };
            item.Click += (_, _) => AddLayer(kind);
            shapes.Items.Add(item);
        }
        _addShape.Flyout = shapes;
        _addImage.Flyout = _imageSources; _imageSources.Opening += (_, _) => FillImageSources();
        _clockHost.Child = _addClock;
        add.Children.Add(_addText); Grid.SetColumn(_addShape, 1); add.Children.Add(_addShape);
        Grid.SetRow(_addImage, 1); add.Children.Add(_addImage); Grid.SetRow(_clockHost, 1); Grid.SetColumn(_clockHost, 1); add.Children.Add(_clockHost);
        Grid.SetRow(add, 4); grid.Children.Add(add);
        return Panel(grid);
    }

    private string? _pendingSelect;
    private void SelectPending() { if (_pendingSelect is { } id) { _pendingSelect = null; SelectLayer(id); } _pendingSelect = null; }

    private void AddLayer(string kind)
    {
        Apply(s => { var (added, id) = GraphicsTemplateEditor.AddLayer(s, kind); _pendingSelect = id; return added; });
        SelectPending();
    }

    private void FillImageSources()
    {
        _imageSources.Items.Clear();
        var import = new MenuFlyoutItem { Text = "Import PNG, JPEG or SVG…" };
        import.Click += async (_, _) => await PickDesignImageAsync();
        _imageSources.Items.Add(import);
        var sources = new List<(string Source, string Label)>();
        if (_session?.TemplateDocument is { } document) {
            try { sources.AddRange(GraphicsDesignerModel.ImageSources(document)); }
            catch (Exception ex) when (ex is InvalidDataException or System.Text.Json.JsonException) { Say(ex.Message); }
        }
        sources.Add(("new-variable", "New image variable (chosen when the graphic is used)"));
        foreach (var (source, label) in sources) {
            var item = new MenuFlyoutItem { Text = label };
            item.Click += (_, _) => { Apply(s => { var (added, id) = GraphicsTemplateEditor.AddImage(s, source); _pendingSelect = id; return added; }); SelectPending(); };
            _imageSources.Items.Add(item);
        }
    }

    private IReadOnlyList<GraphicsDesignerRow> CurrentRows()
    {
        if (_session?.Draft is not { Declarative: true } || _session.TemplateDocument is not { } document) return [];
        try { return GraphicsDesignerModel.Rows(document); }
        catch (Exception ex) when (ex is InvalidDataException or System.Text.Json.JsonException) { Say(ex.Message); return []; }
    }
    private GraphicsDesignerRow? Selected() => _rows.FirstOrDefault(r => r.Id == _session?.SelectedLayerId);

    private void RefreshRows(IReadOnlyList<GraphicsDesignerRow> rows)
    {
        var selected = _session?.SelectedLayerId ?? "";
        // Selection is not in the signature: it restyles the existing rows, so the second tap of a double-click hits the same row.
        var signature = GraphicsDesignerModel.RowsSignature(rows) + "#" + _renaming + "#" + _saving + "#" + CanEdit;
        _rows = rows;
        var row = rows.FirstOrDefault(r => r.Id == selected);
        var editable = CanEdit;
        _duplicate.IsEnabled = _delete.IsEnabled = _group.IsEnabled = editable && row is { Locked: false };
        _ungroup.IsEnabled = editable && row is { Locked: false, Kind: "Group" };
        _addText.IsEnabled = _addShape.IsEnabled = _addImage.IsEnabled = editable;
        var family = _session?.Template?.Family ?? "";
        var clock = family is "clock" or "countdown";
        _addClock.IsEnabled = editable && clock;
        ToolTipService.SetToolTip(_clockHost, clock ? "Add a clock layer that shows the live time." : ClockRefusal);
        if (signature != _rowsSignature) {
            _rowsSignature = signature;
            _rowsPanel.Children.Clear(); _rowParts.Clear();
            for (var i = 0; i < rows.Count; i++) _rowsPanel.Children.Add(Row(rows[i], i));
            if (rows.Count == 0) _rowsPanel.Children.Add(new TextBlock { Text = "Open a design to see its layers.", FontSize = 14, Foreground = StudioBrushes.Muted, TextWrapping = TextWrapping.Wrap });
        }
        foreach (var (id, parts) in _rowParts) StyleRow(parts, id == selected);
    }

    private sealed record RowParts(Grid Row, TextBlock Glyph, TextBlock? Label, TextBlock? Detail);
    private readonly Dictionary<string, RowParts> _rowParts = new(StringComparer.Ordinal);
    private static readonly SolidColorBrush ClearBrush = new(Microsoft.UI.Colors.Transparent);
    private static void StyleRow(RowParts parts, bool selected)
    {
        parts.Row.Background = selected ? StudioBrushes.SurfaceRaised : ClearBrush;
        parts.Row.BorderBrush = selected ? StudioBrushes.Accent : ClearBrush;
        parts.Glyph.Foreground = selected ? StudioBrushes.Accent : StudioBrushes.Muted;
        if (parts.Label is { } label) label.FontWeight = selected ? Microsoft.UI.Text.FontWeights.SemiBold : Microsoft.UI.Text.FontWeights.Normal;
        if (parts.Detail is { } detail) detail.Foreground = selected ? StudioBrushes.Accent : StudioBrushes.Muted;
    }

    private Grid Row(GraphicsDesignerRow row, int index)
    {
        var grid = new Grid { ColumnSpacing = 4, Padding = new Thickness(4 + row.Depth * 14, 3, 2, 3), CornerRadius = new CornerRadius(6), MinHeight = 34, Tag = row,
            Background = ClearBrush, BorderBrush = ClearBrush, BorderThickness = new Thickness(1) };
        foreach (var width in new[] { GridLength.Auto, GridLength.Auto, new GridLength(1, GridUnitType.Star), GridLength.Auto, GridLength.Auto }) grid.ColumnDefinitions.Add(new() { Width = width });
        AutomationProperties.SetName(grid, $"{row.Label}, {row.Kind.ToLowerInvariant()} layer{(row.Locked ? ", locked" : "")}{(row.Visible ? "" : ", hidden")}");
        var editable = CanEdit;

        var eye = IconButton(row.Visible ? "" : "", (row.Visible ? "Hide " : "Show ") + row.Label);
        eye.IsEnabled = editable && !row.VisibilityBound; eye.Opacity = row.Visible ? 1 : .6;
        eye.Click += (_, _) => Apply(s => GraphicsTemplateEditor.SetVisible(s, row.Id, !row.Visible));
        var eyeHost = new Border { Child = eye };
        ToolTipService.SetToolTip(eyeHost, row.VisibilityBound ? "A variable shows or hides this layer, so it cannot be hidden here." : (row.Visible ? "Hide " : "Show ") + row.Label);
        grid.Children.Add(eyeHost);

        var glyph = new TextBlock { Text = row.Kind switch { "Text" => "T", "Clock" => "", "Image" => "", "Group" => "", _ => "■" }, FontSize = row.Kind == "Text" ? 14 : 13,
            FontWeight = Microsoft.UI.Text.FontWeights.Bold, Foreground = StudioBrushes.Muted, VerticalAlignment = VerticalAlignment.Center, Width = 14, TextAlignment = TextAlignment.Center };
        if (row.Kind is not ("Text" or "Shape")) glyph.FontFamily = Icons;
        Grid.SetColumn(glyph, 1); grid.Children.Add(glyph);

        if (_renaming == row.Id) {
            var box = new TextBox { Text = row.Label, MaxLength = 64, FontSize = 14, MinHeight = 28, Padding = new Thickness(6, 2, 6, 2), VerticalAlignment = VerticalAlignment.Center };
            AutomationProperties.SetName(box, "Layer name");
            box.KeyDown += (_, e) => {
                // Always close the box: an unchanged name is a no-op edit that raises no session change.
                if (e.Key == VirtualKey.Enter) { e.Handled = true; var name = box.Text; _renaming = ""; Apply(s => GraphicsTemplateEditor.SetLabel(s, row.Id, name)); _rowsSignature = ""; RefreshAll(); }
                else if (e.Key == VirtualKey.Escape) { e.Handled = true; _renaming = ""; _rowsSignature = ""; RefreshAll(); }
            };
            box.LostFocus += (_, _) => { if (_renaming == row.Id) { _renaming = ""; _rowsSignature = ""; RefreshAll(); } };
            box.Loaded += (_, _) => { box.Focus(FocusState.Programmatic); box.SelectAll(); };
            Grid.SetColumn(box, 2); Grid.SetColumnSpan(box, 2); grid.Children.Add(box);
            _rowParts[row.Id] = new(grid, glyph, null, null);
        } else {
            var label = new TextBlock { Text = row.Label, FontSize = 14, Foreground = StudioBrushes.Text, VerticalAlignment = VerticalAlignment.Center, TextTrimming = TextTrimming.CharacterEllipsis,
                Opacity = row.Visible ? 1 : .6 };
            ToolTipService.SetToolTip(label, row.Label);
            Grid.SetColumn(label, 2); grid.Children.Add(label);
            var detail = new TextBlock { Text = row.Detail, FontSize = 12, Foreground = StudioBrushes.Muted, VerticalAlignment = VerticalAlignment.Center, MaxWidth = 76, TextTrimming = TextTrimming.CharacterEllipsis };
            Grid.SetColumn(detail, 3); grid.Children.Add(detail);
            _rowParts[row.Id] = new(grid, glyph, label, detail);
        }

        var lockButton = IconButton(row.Locked ? "" : "", (row.Locked ? "Unlock " : "Lock ") + row.Label);
        lockButton.Opacity = row.Locked ? 1 : .55; lockButton.IsEnabled = !_saving && _session is { Busy: false };
        lockButton.Click += (_, _) => Apply(s => GraphicsTemplateEditor.Lock(s, row.Id, !row.Locked));
        Grid.SetColumn(lockButton, 4); grid.Children.Add(lockButton);

        grid.Tapped += (_, e) => {
            if (e.OriginalSource is DependencyObject d && (IsInside<ButtonBase>(d, grid) || IsInside<TextBox>(d, grid))) return;
            SelectLayer(row.Id);
        };
        grid.DoubleTapped += (_, e) => {
            if (e.OriginalSource is DependencyObject d && (IsInside<ButtonBase>(d, grid) || IsInside<TextBox>(d, grid))) return;
            if (!CanEdit) return;
            _renaming = row.Id; _rowsSignature = ""; RefreshAll(); e.Handled = true;
        };
        grid.CanDrag = editable && !row.Locked && _renaming != row.Id;
        grid.DragStarting += (_, e) => { _dragId = row.Id; e.Data.SetText(row.Id); e.Data.RequestedOperation = DataPackageOperation.Move; };
        grid.DropCompleted += (_, _) => _dragId = "";
        grid.AllowDrop = true;
        grid.DragOver += (_, e) => {
            var (insertion, onto) = DropPosition(grid, index, row, e);
            e.AcceptedOperation = DropAt(insertion, onto) is null ? DataPackageOperation.None : DataPackageOperation.Move;
            e.DragUIOverride.Caption = onto ? "Into " + row.Label : "Move here";
        };
        grid.Drop += (_, e) => { var (insertion, onto) = DropPosition(grid, index, row, e); DropLayer(insertion, onto); };
        return grid;
    }

    private static Button IconButton(string glyph, string name)
    {
        var button = new Button { Content = glyph, FontFamily = Icons, FontSize = 13, Width = 26, Height = 28, Padding = new Thickness(0), CornerRadius = new CornerRadius(6),
            Background = new SolidColorBrush(Microsoft.UI.Colors.Transparent), BorderThickness = new Thickness(0), Foreground = StudioBrushes.Text, VerticalAlignment = VerticalAlignment.Center };
        AutomationProperties.SetName(button, name);
        return button;
    }

    private static bool IsInside<T>(DependencyObject element, DependencyObject stop) where T : DependencyObject
    {
        for (var cursor = element; cursor is not null && !ReferenceEquals(cursor, stop); cursor = VisualTreeHelper.GetParent(cursor)) if (cursor is T) return true;
        return false;
    }

    // Upper half: before the row; lower half: after it; the middle of a group row: into the group (frontmost).
    private static (int Insertion, bool Onto) DropPosition(FrameworkElement element, int index, GraphicsDesignerRow row, DragEventArgs e)
    {
        var y = e.GetPosition(element).Y; var h = Math.Max(1, element.ActualHeight);
        if (row.Kind == "Group" && y > h * .25 && y < h * .75) return (index, true);
        return (y < h / 2 ? index : index + 1, false);
    }
    private (string Parent, int FrontIndex)? DropAt(int insertion, bool onto) =>
        _dragId.Length == 0 || !CanEdit ? null : GraphicsDesignerModel.DropTarget(_rows, _dragId, insertion, onto);

    private void DropLayer(int insertion, bool onto)
    {
        var id = _dragId; _dragId = "";
        if (id.Length == 0 || _session is null) return;
        if (GraphicsDesignerModel.DropTarget(_rows, id, insertion, onto) is not { } target) { Say("That layer cannot go there: it is locked, or a group cannot go inside itself."); return; }
        // Keep the layer where native drew it on the canvas when it changes group.
        // Only the Typical sample's layout is the draft's own geometry; a sample's flow layout would misplace the layer.
        var node = _session.PreviewSample == "current" ? _session.AuthoringLayout?.Nodes.FirstOrDefault(n => n.Id == id) : null;
        (double X, double Y)? absolute = node is null ? null : (node.X, node.Y);
        var shifted = false;
        if (Apply(s => { var (moved, wasShifted) = GraphicsDesignerModel.MoveLayer(s, id, target.Parent, target.FrontIndex, absolute); shifted = wasShifted; return moved; }, select: id))
            Say(shifted ? "Moved to fit inside the group." : "");
    }
}
