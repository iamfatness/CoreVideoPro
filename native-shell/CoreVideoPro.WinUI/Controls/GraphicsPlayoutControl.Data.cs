using CoreVideoPro.MediaCore.Models;
using CoreVideoPro.MediaCore.Services;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;

namespace CoreVideoPro.WinUI.Controls;

public sealed partial class GraphicsPlayoutControl
{
    private FrameworkElement DataActions(GraphicsPlaylistRow row, GraphicsDataBinding binding, GraphicsWorkspaceLibrary library)
    {
        var panel = new StackPanel { Spacing = 6 };
        var source = library.Document.DataSources.FirstOrDefault(item => item.Id == binding.SourceId);
        panel.Children.Add(new TextBlock
        {
            Text = $"Data · published {binding.Revision} · loaded {source?.Revision.ToString() ?? "unavailable"}",
            TextWrapping = TextWrapping.Wrap
        });
        var reload = StudioBrushes.GhostButton("Load CSV / JSON…");
        var publish = StudioBrushes.AccentButton("Publish data");
        reload.HorizontalAlignment = publish.HorizontalAlignment = HorizontalAlignment.Stretch;
        reload.Click += async (_, _) =>
        {
            if (_attached is not { } studio) return;
            reload.IsEnabled = publish.IsEnabled = false;
            try
            {
                var file = await GraphicsBuilderControl.PickDataFileAsync();
                if (file is null || _attached != studio) return;
                var data = await GraphicsLocalData.ReadAsync(file.Path, binding.KeyColumn);
                GraphicsDataTable.Fields(data, binding);
                if (studio.GraphicsPlayout.Row(row.Id)?.DataBinding != binding) return;
                var staged = await library.SaveLocalDataAsync(binding.SourceId, source?.Name ?? row.Name, data);
                _status.Text = staged is null ? library.Status : $"Data revision {staged.Revision} loaded. Choose Publish data to apply it.";
            }
            catch (Exception ex) when (ex is not OutOfMemoryException) { _status.Text = ex.Message; }
            finally { reload.IsEnabled = publish.IsEnabled = true; }
        };
        publish.Click += async (_, _) =>
        {
            if (_attached is not { } studio) return;
            reload.IsEnabled = publish.IsEnabled = false;
            try
            {
                await _rowText.Flush();
                var result = await studio.GraphicsPlayout.PublishDataAsync(row.Id);
                _status.Text = result.Reason;
            }
            catch (Exception ex) when (ex is not OutOfMemoryException) { _status.Text = ex.Message; }
            finally { reload.IsEnabled = publish.IsEnabled = true; }
        };
        panel.Children.Add(reload);
        panel.Children.Add(publish);
        return panel;
    }
}
