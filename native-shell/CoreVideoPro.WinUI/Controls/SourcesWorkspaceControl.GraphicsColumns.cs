using CoreVideoPro.MediaCore.Services;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using static CoreVideoPro.WinUI.Controls.SourcesTableRowControl;

namespace CoreVideoPro.WinUI.Controls;

public sealed partial class SourcesWorkspaceControl
{
    private readonly Grid _graphicsColumnsTable = new() { ColumnSpacing = 12, RowSpacing = 6, Padding = new Thickness(8) };
    private readonly Expander _graphicsColumns = new() { Header = "Graphics columns", Visibility = Visibility.Collapsed, HorizontalAlignment = HorizontalAlignment.Stretch };
    private GraphicsWorkspaceLibrary? _graphicsColumnsLibrary;
    private string _graphicsColumnsSignature = "";

    private Expander BuildGraphicsColumns()
    {
        _graphicsColumns.Content = new ScrollViewer
        {
            Content = _graphicsColumnsTable,
            HorizontalScrollMode = ScrollMode.Auto,
            HorizontalScrollBarVisibility = ScrollBarVisibility.Auto,
            VerticalScrollMode = ScrollMode.Disabled
        };
        return _graphicsColumns;
    }

    private async Task LoadGraphicsColumnsAsync(GraphicsWorkspaceLibrary library)
    {
        try { await library.LoadAsync(); }
        catch (Exception ex) when (ex is not OutOfMemoryException)
        {
            if (ReferenceEquals(library, _graphicsColumnsLibrary)) _status.Text = ex.Message;
        }
    }

    private void GraphicsColumnsChanged(object? sender, EventArgs e) => RefreshGraphicsColumns();

    private void RefreshGraphicsColumns()
    {
        var columns = _graphicsColumnsLibrary?.Document.SourceColumns ?? [];
        _graphicsColumns.Visibility = columns.Count == 0 ? Visibility.Collapsed : Visibility.Visible;
        if (columns.Count == 0) { _graphicsColumnsSignature = ""; return; }
        var slots = _rows.Values.Select(row => row.Input).OrderBy(input => input.SlotNumber).ToArray();
        var signature = string.Join('\u001f', columns.SelectMany(column => new[] { column.Id, column.Label }
            .Concat(slots.Select(slot => column.SlotValues.GetValueOrDefault(slot.SlotNumber, ""))))) +
            string.Join('\u001f', slots.Select(slot => $"{slot.SlotNumber}:{slot.DisplayName}"));
        if (_graphicsColumnsSignature == signature) return;
        _graphicsColumnsSignature = signature;
        var table = _graphicsColumnsTable;
        table.Children.Clear(); table.RowDefinitions.Clear(); table.ColumnDefinitions.Clear();
        table.ColumnDefinitions.Add(new() { Width = new GridLength(140) });
        foreach (var column in columns) table.ColumnDefinitions.Add(new() { Width = new GridLength(140) });

        void AddCell(string value, int row, int column)
        {
            var cell = Text(value, 12);
            cell.TextTrimming = TextTrimming.CharacterEllipsis;
            ToolTipService.SetToolTip(cell, value);
            Grid.SetRow(cell, row); Grid.SetColumn(cell, column); table.Children.Add(cell);
        }

        table.RowDefinitions.Add(new() { Height = GridLength.Auto });
        AddCell("Sources slot", 0, 0);
        for (var column = 0; column < columns.Count; column++) AddCell(columns[column].Label, 0, column + 1);
        foreach (var slot in slots)
        {
            var row = table.RowDefinitions.Count;
            table.RowDefinitions.Add(new() { Height = GridLength.Auto });
            AddCell($"{slot.SlotNumber:00} {slot.DisplayName}", row, 0);
            for (var column = 0; column < columns.Count; column++)
                AddCell(columns[column].SlotValues.GetValueOrDefault(slot.SlotNumber, ""), row, column + 1);
        }
    }
}
