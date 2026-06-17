using System.Collections;
using CoreVideoPro.WinUI.Models;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;

namespace CoreVideoPro.WinUI.Controls;

public sealed partial class MultiviewGridHost : UserControl
{
    public const int DefaultColumns = 8;
    public const int DefaultRows = 8;

    public static readonly DependencyProperty TilesProperty =
        DependencyProperty.Register(nameof(Tiles), typeof(IEnumerable), typeof(MultiviewGridHost),
            new PropertyMetadata(null, OnTilesChanged));

    public static readonly DependencyProperty GridColumnsProperty =
        DependencyProperty.Register(nameof(GridColumns), typeof(int), typeof(MultiviewGridHost),
            new PropertyMetadata(DefaultColumns, OnGridShapeChanged));

    public static readonly DependencyProperty GridRowsProperty =
        DependencyProperty.Register(nameof(GridRows), typeof(int), typeof(MultiviewGridHost),
            new PropertyMetadata(DefaultRows, OnGridShapeChanged));

    public MultiviewGridHost()
    {
        InitializeComponent();
        Loaded += (_, _) => UpdateCellMetrics();
    }

    public IEnumerable? Tiles
    {
        get => (IEnumerable?)GetValue(TilesProperty);
        set => SetValue(TilesProperty, value);
    }

    public int GridColumns
    {
        get => (int)GetValue(GridColumnsProperty);
        set => SetValue(GridColumnsProperty, value);
    }

    public int GridRows
    {
        get => (int)GetValue(GridRowsProperty);
        set => SetValue(GridRowsProperty, value);
    }

    private static void OnTilesChanged(DependencyObject sender, DependencyPropertyChangedEventArgs args)
    {
        if (sender is MultiviewGridHost host)
        {
            host.TileRepeater.ItemsSource = args.NewValue as IEnumerable;
        }
    }

    private static void OnGridShapeChanged(DependencyObject sender, DependencyPropertyChangedEventArgs args)
    {
        if (sender is MultiviewGridHost host)
        {
            host.UpdateCellMetrics();
        }
    }

    private void OnSizeChanged(object sender, SizeChangedEventArgs e) => UpdateCellMetrics();

    private void UpdateCellMetrics()
    {
        var columns = Math.Max(1, GridColumns);
        var rows = Math.Max(1, GridRows);
        TileLayout.MaximumRowsOrColumns = columns;

        var width = GridSurface.ActualWidth;
        var height = GridSurface.ActualHeight;
        if (width <= 0 || height <= 0)
        {
            return;
        }

        var columnGap = TileLayout.MinColumnSpacing;
        var rowGap = TileLayout.MinRowSpacing;
        var cellWidth = (width - (columnGap * (columns - 1))) / columns;
        var cellHeight = (height - (rowGap * (rows - 1))) / rows;
        TileLayout.MinItemWidth = Math.Max(24, cellWidth);
        TileLayout.MinItemHeight = Math.Max(14, cellHeight);
    }
}