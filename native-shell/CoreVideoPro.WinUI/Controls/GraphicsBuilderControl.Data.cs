using CoreVideoPro.MediaCore.Services;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Windows.Storage;
using Windows.Storage.Pickers;

namespace CoreVideoPro.WinUI.Controls;

public sealed partial class GraphicsBuilderControl
{
    private async Task ShowDataTableDialogAsync(GraphicsDataSource? remote = null, string? remoteKey = null)
    {
        if (_designBusy || _attached is not { } studio || XamlRoot is null) return;
        _designBusy = true;
        try
        {
            var file = remote is null ? await PickDataFileAsync() : null;
            if ((remote is null && file is null) || _attached != studio) return;
            var data = remote?.Data ?? await GraphicsLocalData.ReadAsync(file!.Path, null);
            var displayName = remote?.Name ?? file!.DisplayName;
            var panel = new StackPanel { Spacing = 8 };
            var name = new TextBox { Header = "Design name", Text = displayName[..Math.Min(64, displayName.Length)], MaxLength = 64 };
            var key = new ComboBox { Header = "Row key", ItemsSource = new[] { "Use file order" }.Concat(data.Columns.Select(c => c.Id)).ToArray(), SelectedIndex = 0 };
            var sort = new ComboBox { Header = "Leaderboard order", ItemsSource = new[] { "Table · file order" }.Concat(data.Columns.Where(c => c.Kind == "number").Select(c => c.Id)).ToArray(), SelectedIndex = 0 };
            var limit = new NumberBox { Header = "Visible rows", Minimum = 1, Maximum = 12, Value = 12, SpinButtonPlacementMode = NumberBoxSpinButtonPlacementMode.Compact };
            var columns = data.Columns.Select((column, index) => new CheckBox { Content = $"{column.Id} · {column.Kind}", Tag = column.Id, IsChecked = index < 2 }).ToArray();
            var status = new TextBlock { TextWrapping = TextWrapping.Wrap };
            var summary = new TextBlock { Text = $"{data.Rows.Length} rows · {data.Columns.Length} columns. Selected columns appear in source order." };
            if (remote is not null)
            {
                key.ItemsSource = new[] { remoteKey };
                key.SelectedIndex = 0;
                key.IsEnabled = false;
            }
            foreach (var control in new FrameworkElement[] { name, summary, key, sort, limit }) panel.Children.Add(control);
            foreach (var column in columns) panel.Children.Add(column);
            panel.Children.Add(status);
            var dialog = new ContentDialog
            {
                XamlRoot = XamlRoot, RequestedTheme = ActualTheme, Title = "Create data table",
                Content = new ScrollViewer { Content = panel, MaxHeight = 440 },
                PrimaryButtonText = "Create design", CloseButtonText = "Cancel"
            };
            dialog.PrimaryButtonClick += async (_, args) =>
            {
                var deferral = args.GetDeferral();
                try
                {
                    var chosen = columns.Where(column => column.IsChecked == true).Select(column => (string)column.Tag).ToArray();
                    var keyColumn = remoteKey ?? (key.SelectedIndex > 0 ? key.SelectedItem as string : null);
                    var sourceId = remote?.Id ?? "data-" + Guid.NewGuid().ToString("N");
                    var binding = new GraphicsDataBinding(sourceId, keyColumn, chosen,
                        sort.SelectedIndex > 0 ? sort.SelectedItem as string : null, double.IsFinite(limit.Value) ? (int)Math.Round(limit.Value) : 12, remote?.Revision ?? 1) { Remote = remote is not null };
                    var source = remote ?? new GraphicsDataSource(sourceId, name.Text.Trim(), 1, DateTimeOffset.UtcNow, data.WithKey(keyColumn));
                    var result = await studio.CreateGraphicsDataTableAsync(name.Text, source, binding);
                    args.Cancel = !result.Ok;
                    status.Text = _designStatus.Text = result.Message;
                }
                catch (Exception ex) when (ex is not OutOfMemoryException)
                {
                    args.Cancel = true;
                    status.Text = ex.Message;
                }
                finally { deferral.Complete(); }
            };
            await dialog.ShowAsync();
        }
        catch (Exception ex) when (ex is not OutOfMemoryException) { _designStatus.Text = ex.Message; }
        finally { _designBusy = false; _designsSignature = ""; RefreshDesigns(); }
    }

    private async Task ShowCrawlDialogAsync(GraphicsDataSource? remote = null, string? remoteKey = null)
    {
        if (_designBusy || _attached is not { } studio || XamlRoot is null) return;
        _designBusy = true;
        try
        {
            var file = remote is null ? await PickDataFileAsync() : null;
            if ((remote is null && file is null) || _attached != studio) return;
            var data = remote?.Data ?? await GraphicsLocalData.ReadAsync(file!.Path, null);
            var displayName = remote?.Name ?? file!.DisplayName;
            var textColumns = data.Columns.Where(item => item.Kind == "text").Select(item => item.Id).ToArray();
            if (textColumns.Length == 0) throw new InvalidDataException("The crawl needs a text column.");
            var panel = new StackPanel { Spacing = 8 };
            var name = new TextBox { Header = "Design name", Text = displayName[..Math.Min(64, displayName.Length)], MaxLength = 64 };
            var column = new ComboBox { Header = "Text column", ItemsSource = textColumns, SelectedIndex = 0 };
            var direction = new ComboBox { Header = "Direction", ItemsSource = new[] { "Left", "Right" }, SelectedIndex = 0 };
            var mode = new ComboBox { Header = "Playback", ItemsSource = new[] { "Loop", "Single pass" }, SelectedIndex = 0 };
            var speed = new NumberBox { Header = "Speed (px/s)", Minimum = 1, Maximum = 1200, Value = 120 };
            var gap = new NumberBox { Header = "Gap (px)", Minimum = 0, Maximum = 512, Value = 48 };
            var limit = new NumberBox { Header = "Item limit", Minimum = 1, Maximum = GraphicsCrawlData.MaxItems, Value = GraphicsCrawlData.MaxItems };
            var sample = new TextBlock { TextWrapping = TextWrapping.Wrap, MaxLines = 4, TextTrimming = TextTrimming.CharacterEllipsis };
            var status = new TextBlock { TextWrapping = TextWrapping.Wrap };
            foreach (var control in new FrameworkElement[] { name, column, sample, direction, mode, speed, gap, limit, status })
                panel.Children.Add(control);
            int SelectedLimit() => double.IsFinite(limit.Value) ? (int)Math.Round(limit.Value) : GraphicsCrawlData.MaxItems;
            void RefreshSample()
            {
                if (column.SelectedItem is not string selected) return;
                var items = GraphicsCrawlData.Items(data, selected, SelectedLimit());
                sample.Text = $"{items.Length} items in source order\n" + string.Join("  •  ", items.Take(4));
            }
            column.SelectionChanged += (_, _) => RefreshSample();
            limit.ValueChanged += (_, _) => RefreshSample();
            RefreshSample();
            var dialog = new ContentDialog
            {
                XamlRoot = XamlRoot, RequestedTheme = ActualTheme, Title = "Create data crawl",
                Content = new ScrollViewer { Content = panel, MaxHeight = 440 },
                PrimaryButtonText = "Create design", CloseButtonText = "Cancel"
            };
            dialog.PrimaryButtonClick += async (_, args) =>
            {
                var deferral = args.GetDeferral();
                try
                {
                    var sourceId = remote?.Id ?? "data-" + Guid.NewGuid().ToString("N");
                    var source = remote ?? new GraphicsDataSource(sourceId, name.Text.Trim(), 1, DateTimeOffset.UtcNow, data);
                    var binding = new GraphicsDataBinding(sourceId, remoteKey, [(string)column.SelectedItem], null, SelectedLimit(), source.Revision)
                    {
                        Remote = remote is not null,
                        Crawl = new(speed.Value, gap.Value, direction.SelectedIndex == 1 ? "right" : "left",
                            mode.SelectedIndex == 1 ? "single" : "loop")
                    };
                    var result = await studio.CreateGraphicsDataTableAsync(name.Text, source, binding);
                    args.Cancel = !result.Ok;
                    status.Text = _designStatus.Text = result.Message;
                }
                catch (Exception ex) when (ex is not OutOfMemoryException)
                {
                    args.Cancel = true;
                    status.Text = ex.Message;
                }
                finally { deferral.Complete(); }
            };
            await dialog.ShowAsync();
        }
        catch (Exception ex) when (ex is not OutOfMemoryException) { _designStatus.Text = ex.Message; }
        finally { _designBusy = false; _designsSignature = ""; RefreshDesigns(); }
    }

    internal static async Task<StorageFile?> PickDataFileAsync()
    {
        var picker = new FileOpenPicker();
        picker.FileTypeFilter.Add(".csv");
        picker.FileTypeFilter.Add(".json");
        WinRT.Interop.InitializeWithWindow.Initialize(picker, App.MainWindowHandle);
        return await picker.PickSingleFileAsync();
    }
}
