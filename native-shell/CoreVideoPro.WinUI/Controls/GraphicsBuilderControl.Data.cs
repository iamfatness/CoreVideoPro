using CoreVideoPro.MediaCore.Services;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Windows.Storage;
using Windows.Storage.Pickers;

namespace CoreVideoPro.WinUI.Controls;

public sealed partial class GraphicsBuilderControl
{
    private async Task ShowDataTableDialogAsync()
    {
        if (_designBusy || _attached is not { } studio || XamlRoot is null) return;
        _designBusy = true;
        try
        {
            var file = await PickDataFileAsync();
            if (file is null || _attached != studio) return;
            var data = await GraphicsLocalData.ReadAsync(file.Path, null);
            var panel = new StackPanel { Spacing = 8 };
            var name = new TextBox { Header = "Design name", Text = file.DisplayName[..Math.Min(64, file.DisplayName.Length)], MaxLength = 64 };
            var key = new ComboBox { Header = "Row key", ItemsSource = new[] { "Use file order" }.Concat(data.Columns.Select(c => c.Id)).ToArray(), SelectedIndex = 0 };
            var sort = new ComboBox { Header = "Leaderboard order", ItemsSource = new[] { "Table · file order" }.Concat(data.Columns.Where(c => c.Kind == "number").Select(c => c.Id)).ToArray(), SelectedIndex = 0 };
            var limit = new NumberBox { Header = "Visible rows", Minimum = 1, Maximum = 12, Value = 12, SpinButtonPlacementMode = NumberBoxSpinButtonPlacementMode.Compact };
            var columns = data.Columns.Select((column, index) => new CheckBox { Content = $"{column.Id} · {column.Kind}", Tag = column.Id, IsChecked = index < 2 }).ToArray();
            var status = new TextBlock { TextWrapping = TextWrapping.Wrap };
            var summary = new TextBlock { Text = $"{data.Rows.Length} rows · {data.Columns.Length} columns. Selected columns appear in file order." };
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
                    var keyColumn = key.SelectedIndex > 0 ? key.SelectedItem as string : null;
                    var sourceId = "data-" + Guid.NewGuid().ToString("N");
                    var binding = new GraphicsDataBinding(sourceId, keyColumn, chosen,
                        sort.SelectedIndex > 0 ? sort.SelectedItem as string : null, double.IsFinite(limit.Value) ? (int)Math.Round(limit.Value) : 12, 1);
                    var source = new GraphicsDataSource(sourceId, name.Text.Trim(), 1, DateTimeOffset.UtcNow, data.WithKey(keyColumn));
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
