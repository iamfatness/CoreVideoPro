using System.Collections.Specialized;
using System.ComponentModel;
using CoreVideoPro.WinUI.Services;
using CoreVideoPro.WinUI.ViewModels;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Data;
using Microsoft.UI.Xaml.Automation;
using Microsoft.UI.Xaml.Media;
using static CoreVideoPro.WinUI.Controls.SourcesTableRowControl;

namespace CoreVideoPro.WinUI.Controls;

/// <summary>Table-first operator workspace over the existing stable input models.</summary>
public sealed partial class SourcesWorkspaceControl : UserControl
{
    internal SourcesWorkspaceState State { get; } = new();
    private readonly ISourcesWorkspacePreferenceStore _preferences;
    private readonly StackPanel _body = new() { Spacing = 10 };
    private readonly Grid _workspace = new() { ColumnSpacing = 14, RowSpacing = 14 };
    private readonly StackPanel _table = new() { Spacing = 2 };
    private readonly Grid _header = new() { ColumnSpacing = 6, Padding = new(8), Background = Brush(35, 55, 66) };
    private readonly StackPanel _rowsPanel = new() { Spacing = 2 };
    private readonly Dictionary<int, SourcesTableRowControl> _rows = [];
    private readonly Border _inspector = new() { Padding = new(16), Background = Brush(26, 44, 55), CornerRadius = new(8) };
    private readonly Button _toggleInspector;
    private readonly Button _grade;
    private readonly TextBlock _empty = Text("No matching inputs. Choose All sources or clear the search to see empty slots.");
    private readonly TextBlock _summary = Text("");
    private readonly TextBlock _status = Text("Select Inspect for details. Selection does not assign a source, change MV/ISO or Take.", 11);
    private readonly DropDownButton _picker = new() { Name = "SelectedSourcePicker", HorizontalAlignment = HorizontalAlignment.Stretch };
    private StudioViewModel? _owner;
    private bool _queued;
    public event EventHandler? CatalogRequested;

    public SourcesWorkspaceControl() : this(new SourcesWorkspacePreferenceStore()) { }
    internal SourcesWorkspaceControl(ISourcesWorkspacePreferenceStore preferences)
    {
        RequestedTheme = ElementTheme.Dark;
        _preferences = preferences; State.Columns = preferences.Load(); Content = _body;
        _body.Children.Add(Text("Show inputs", 18));
        _body.Children.Add(Text("1 Assign inputs / Multiview   →   2 Route outputs   →   3 Compose scenes / Take", 11));
        var filters = new Grid { ColumnSpacing = 12 };
        filters.ColumnDefinitions.Add(new() { Width = new(1, GridUnitType.Star) });
        filters.ColumnDefinitions.Add(new() { Width = new(170) });
        var search = new TextBox { Background = Brush(15,25,31), Foreground = Brush(231,240,243), Name = "SourcesSearch", PlaceholderText = "Find an input or source", Header = "Find source" };
        search.TextChanged += (_, _) => State.Query = search.Text;
        var category = new ComboBox { Background = Brush(15,25,31), Foreground = Brush(231,240,243), Name = "SourcesCategory", Header = "Category", ItemsSource = SourcesWorkspaceState.Categories, SelectedIndex = 0, HorizontalAlignment = HorizontalAlignment.Stretch };
        category.SelectionChanged += (_, _) => State.Category = category.SelectedItem as string ?? "All sources";
        filters.Children.Add(search); Grid.SetColumn(category, 1); filters.Children.Add(category); _body.Children.Add(filters);
        var tools = new Grid { ColumnSpacing = 8, RowSpacing = 8 };
        tools.ColumnDefinitions.Add(new() { Width = new(1, GridUnitType.Star) }); tools.ColumnDefinitions.Add(new() { Width = new(1, GridUnitType.Star) });
        var leftTools = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 6 };
        var columns = new DropDownButton { Background = Brush(32,49,59), Foreground = Brush(231,240,243), Content = "Columns", FontSize = 12 };
        var columnsFlyout = new MenuFlyout(); columns.Flyout = columnsFlyout;
        foreach (var key in new[] { "Type", "Signal", "Audio" })
        {
            var item = new ToggleMenuFlyoutItem { Text = key, IsChecked = key == "Type" ? State.Columns.ShowType : key == "Signal" ? State.Columns.ShowSignal : State.Columns.ShowAudio };
            item.Click += (_, _) =>
            {
                State.Columns = key switch { "Type" => State.Columns with { ShowType = item.IsChecked }, "Signal" => State.Columns with { ShowSignal = item.IsChecked }, _ => State.Columns with { ShowAudio = item.IsChecked } };
                try { _preferences.Save(State.Columns); _status.Text = "Column preferences saved. Production settings unchanged."; }
                catch (Exception ex) when (ex is IOException or UnauthorizedAccessException)
                { _status.Text = "Column layout changed for this session; it could not be saved."; }
            };
            columnsFlyout.Items.Add(item);
        }
        leftTools.Children.Add(columns);
        var quality = new DropDownButton { Background = Brush(32,49,59), Foreground = Brush(231,240,243), Content = "Zoom quality", FontSize = 12 };
        quality.Flyout = new Flyout { Content = BuildQuality() }; leftTools.Children.Add(quality);
        leftTools.Children.Add(Button("Source catalog", () => CatalogRequested?.Invoke(this, EventArgs.Empty)));
        var rightTools = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 6, HorizontalAlignment = HorizontalAlignment.Right };
        _grade = Button("Grade selected…", () => Grade(State.SelectedInput)); _grade.Name = "GradeSelectedSource";
        _toggleInspector = Button("Open inspector", () => State.InspectorOpen = !State.InspectorOpen); _toggleInspector.Name = "ToggleSourceInspector";
        rightTools.Children.Add(_grade); rightTools.Children.Add(_toggleInspector);
        tools.Children.Add(leftTools); Grid.SetColumn(rightTools, 1); tools.Children.Add(rightTools); _body.Children.Add(tools);
        SizeChanged += (_, _) =>
        {
            var stacked = ActualWidth < 800;
            if (stacked && tools.RowDefinitions.Count == 0) { tools.RowDefinitions.Add(new()); tools.RowDefinitions.Add(new()); }
            Grid.SetColumn(rightTools, stacked ? 0 : 1); Grid.SetRow(rightTools, stacked ? 1 : 0);
            rightTools.HorizontalAlignment = stacked ? HorizontalAlignment.Left : HorizontalAlignment.Right;
            LayoutWorkspace();
        };
        _summary.FontSize = 11; _summary.TextWrapping = TextWrapping.Wrap; _body.Children.Add(_summary);
        var labels = new[] { "#", "Source", "Type", "Format", "FPS", "Signal", "Audio", "MV", "ISO", "Actions" };
        for (var i = 0; i < labels.Length; i++) { _header.ColumnDefinitions.Add(new()); var label = Text(labels[i], 10); Grid.SetColumn(label, i); _header.Children.Add(label); }
        _table.Children.Add(_header); _table.Children.Add(_rowsPanel); _empty.TextWrapping = TextWrapping.Wrap; _table.Children.Add(_empty);
        var tableNote = Text("Observed source format/FPS. A stale source is not advancing video. ISO selection is separate from writer state.", 11); tableNote.TextWrapping = TextWrapping.Wrap; _table.Children.Add(tableNote);
        _workspace.ColumnDefinitions.Add(new() { Width = new(1, GridUnitType.Star) }); _workspace.ColumnDefinitions.Add(new() { Width = new(0) });
        _workspace.RowDefinitions.Add(new() { Height = GridLength.Auto }); _workspace.RowDefinitions.Add(new() { Height = GridLength.Auto });
        _workspace.Children.Add(_table); _inspector.Child = BuildInspector(); _workspace.Children.Add(_inspector); _body.Children.Add(_workspace);
        _table.SizeChanged += (_, _) => ApplyColumns();
        _status.TextWrapping = TextWrapping.Wrap; _body.Children.Add(_status);
        State.PropertyChanged += OnStateChanged;
        Loaded += (_, _) => Attach(); Unloaded += (_, _) => Detach();
        ApplyState();
    }

    public StudioViewModel? Studio { get => (StudioViewModel?)GetValue(StudioProperty); set => SetValue(StudioProperty, value); }
    public static readonly DependencyProperty StudioProperty = DependencyProperty.Register(nameof(Studio), typeof(StudioViewModel),
        typeof(SourcesWorkspaceControl), new PropertyMetadata(null, (sender, _) => { if (sender is SourcesWorkspaceControl { IsLoaded: true } control) control.Attach(); }));

    private void Attach()
    {
        if (ReferenceEquals(_owner, Studio) && _rows.Count > 0) return;
        Detach(); _owner = Studio;
        if (_owner is null) { State.SelectedInput = null; State.InspectorOpen = false; ApplyState(); return; }
        _owner.ShowInputEditors.CollectionChanged += OnEditorsChanged;
        ReconcileRows();
    }
    private void Detach()
    {
        if (_owner is not null) _owner.ShowInputEditors.CollectionChanged -= OnEditorsChanged;
        foreach (var row in _rows.Values) row.Dispose(); _rows.Clear(); _rowsPanel.Children.Clear(); _owner = null;
    }
    private void OnEditorsChanged(object? sender, NotifyCollectionChangedEventArgs e)
    {
        if (_queued) return; _queued = true;
        DispatcherQueue.TryEnqueue(() => { _queued = false; if (IsLoaded) ReconcileRows(); });
    }
    private void ReconcileRows()
    {
        if (_owner is null) return;
        var inputs = _owner.ShowInputEditors.OrderBy(row => row.SlotNumber).ToArray();
        foreach (var pair in _rows.ToArray())
            if (!inputs.Any(input => ReferenceEquals(input, pair.Value.Input))) { pair.Value.Dispose(); _rowsPanel.Children.Remove(pair.Value); _rows.Remove(pair.Key); }
        foreach (var input in inputs)
            if (!_rows.ContainsKey(input.SlotNumber))
            {
                var row = new SourcesTableRowControl(input, State.Inspect, Grade, ApplyState);
                _rows[input.SlotNumber] = row;
                _rowsPanel.Children.Insert(inputs.TakeWhile(item => item != input).Count(item => _rows.ContainsKey(item.SlotNumber)), row);
            }
        if (State.SelectedInput is null || !inputs.Contains(State.SelectedInput)) State.SelectedInput = inputs.FirstOrDefault();
        ApplyState();
    }
    private void OnStateChanged(object? sender, PropertyChangedEventArgs e) { ApplyState(); if (e.PropertyName is nameof(SourcesWorkspaceState.InspectorOpen)) LayoutWorkspace(); }
    private void ApplyState()
    {
        var shown = 0;
        foreach (var row in _rows.Values) { row.Visibility = State.Matches(row.Input) ? Visibility.Visible : Visibility.Collapsed; shown += row.Visibility == Visibility.Visible ? 1 : 0; row.SetSelected(ReferenceEquals(State.SelectedInput, row.Input)); }
        _empty.Visibility = shown == 0 ? Visibility.Visible : Visibility.Collapsed;
        _summary.Text = $"{_rows.Values.Count(row => row.Input.IsAssigned)}/{_rows.Count} assigned · {_rows.Values.Count(row => row.Input.InShow && row.Input.IsAssigned)} in MV · {_rows.Values.Count(row => row.Input.IsoEnabled)} ISO selected · {shown} match filter";
        _grade.IsEnabled = State.SelectedInput?.IsAssigned == true;
        _toggleInspector.Content = State.InspectorOpen ? "Close inspector" : "Open inspector";
        _inspector.Visibility = State.InspectorOpen ? Visibility.Visible : Visibility.Collapsed;
        _picker.Tag = State.SelectedInput; ApplyColumns();
    }
    private void LayoutWorkspace()
    {
        var side = State.InspectorOpen && ActualWidth >= 1500;
        _workspace.ColumnDefinitions[1].Width = new(side ? 340 : 0);
        Grid.SetColumn(_inspector, side ? 1 : 0); Grid.SetRow(_inspector, side ? 0 : 1);
        ApplyColumns();
    }
    private void ApplyColumns()
    {
        var width = _table.ActualWidth > 0 ? _table.ActualWidth : ActualWidth;
        var columns = SourcesTableRowControl.ColumnWidths(width, State.Columns);
        for (var i = 0; i < columns.Length; i++) { _header.ColumnDefinitions[i].Width = columns[i] < 0 ? new(1, GridUnitType.Star) : new(columns[i]); _header.Children[i].Visibility = columns[i] == 0 ? Visibility.Collapsed : Visibility.Visible; }
        foreach (var row in _rows.Values) row.Layout(width, State.Columns);
    }
    internal void Grade(ShowInputSlotViewModel? input)
    {
        if (input?.SourceColorGradeId is not { Length: > 0 } id || Studio is null) return;
        Studio.OpenColorGradeEditorCommand.Execute(id);
    }
    private StackPanel BuildQuality()
    {
        var panel = new StackPanel { Spacing = 10, Width = 280 };
        panel.Children.Add(Text("Requested Zoom quality", 15));
        foreach (var fps in new[] { false, true })
        {
            var picker = new DropDownButton { HorizontalAlignment = HorizontalAlignment.Stretch };
            Bind(picker, ContentControl.ContentProperty, this, fps ? "Studio.ZoomCameraMaxFps" : "Studio.ZoomCameraMaxResolution");
            var menu = new MenuFlyout(); menu.Opening += (_, _) =>
            {
                menu.Items.Clear(); if (Studio is not { } studio) return;
                var values = fps ? studio.ZoomCameraMaxFpsChoices.Select(value => value.ToString()).ToArray() : studio.ZoomCameraMaxChoices.ToArray();
                foreach (var value in values)
                {
                    var item = new MenuFlyoutItem { Text = value + (fps ? " fps" : "") };
                    item.Click += (_, _) => { if (!ReferenceEquals(studio, Studio)) return; if (fps) studio.ZoomCameraMaxFps = int.Parse(value); else studio.ZoomCameraMaxResolution = value; };
                    menu.Items.Add(item);
                }
            };
            picker.Flyout = menu; panel.Children.Add(Text(fps ? "Max Zoom guest FPS (local)" : "Preferred Zoom camera max", 11)); panel.Children.Add(picker);
            var readout = Text("", 11); readout.TextWrapping = TextWrapping.Wrap;
            Bind(readout, TextBlock.TextProperty, this, fps ? "Studio.ZoomCameraMaxFpsStatus" : "Studio.ZoomCameraMaxStatus"); panel.Children.Add(readout);
        }
        var note = Text("Request ceilings are not delivered measurements. Existing join/rejoin rules still apply.", 11); note.TextWrapping = TextWrapping.Wrap; panel.Children.Add(note); return panel;
    }
}
