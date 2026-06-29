using System.Collections;
using System.Collections.Generic;
using System.Collections.ObjectModel;
using System.Linq;
using CoreVideoPro.WinUI.Models;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;

namespace CoreVideoPro.WinUI.Controls;

public sealed partial class MultiviewGridHost : UserControl
{
    public const int DefaultColumns = 8;
    public const int DefaultRows = 8;

    // Stable backing collection for the tile repeater. We NEVER swap the ItemsSource (that
    // resets the ItemsRepeater and unloads/reloads every tile's VideoSurfaceHost GPU swap
    // chain -> churn storm -> CoreMessagingXP fail-fast). Instead we sync this collection in
    // place: Replace only the slots that actually changed (different participant or GPU
    // handle), so unchanged GPU tiles keep presenting (content self-refreshes via the keyed
    // mutex) and only join/leave add/remove elements.
    private readonly ObservableCollection<ParticipantSurfaceTile> _items = new();

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
        TileRepeater.ItemsSource = _items;  // set ONCE; never reassigned
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
            host.SyncItems(args.NewValue as IEnumerable);
        }
    }

    // Reconcile _items with the incoming tiles WITHOUT swapping the ItemsSource. Replace
    // only slots that materially changed; ItemsRepeater recycles the element on Replace
    // (it does NOT unload/reload it), so a GPU tile's VideoSurfaceHost stays alive.
    private void SyncItems(IEnumerable? incoming)
    {
        var next = incoming?.Cast<ParticipantSurfaceTile>().ToList() ?? new List<ParticipantSurfaceTile>();

        for (var i = 0; i < next.Count; i++)
        {
            if (i < _items.Count)
            {
                if (!SameSlot(_items[i], next[i]))
                {
                    _items[i] = next[i];
                }
            }
            else
            {
                _items.Add(next[i]);
            }
        }

        while (_items.Count > next.Count)
        {
            _items.RemoveAt(_items.Count - 1);
        }
    }

    // Two tiles are "the same slot" (skip the Replace) when they show the same participant
    // in the same emptiness/position AND present the same live GPU handle — the keyed-mutex
    // texture refreshes its own content, so no binding update is needed and the host must
    // not be disturbed. Tiles WITHOUT a GPU handle (base64/slate) always replace so their
    // pixels/metadata refresh.
    private static bool SameSlot(ParticipantSurfaceTile a, ParticipantSurfaceTile b)
    {
        if (a.IsEmpty || b.IsEmpty)
        {
            return a.IsEmpty && b.IsEmpty;
        }

        var ha = a.Surface?.PendingSharedHandle;
        var hb = b.Surface?.PendingSharedHandle;
        if (ha is not { IsValid: true } || hb is not { IsValid: true })
        {
            return false;  // no stable GPU handle on one side -> replace to refresh
        }

        return a.Participant?.Id == b.Participant?.Id &&
               a.SourceIndex == b.SourceIndex &&
               ha.NtHandle == hb.NtHandle;
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