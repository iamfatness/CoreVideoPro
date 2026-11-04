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
                await _rowText.Flush();
                _status.Text = (await studio.GraphicsPlayout.StageDataAsync(row.Id, binding, data)).Reason;
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
        if (binding.Crawl is not null)
        {
            if (source is not null && source.Revision > binding.Revision)
            {
                try
                {
                    var items = GraphicsCrawlData.Items(source.Data, binding.Columns[0], binding.Limit);
                    panel.Children.Add(new TextBlock
                    {
                        Text = $"Loaded {items.Length} {(items.Length == 1 ? "item" : "items")} · " +
                            string.Join("  •  ", items.Take(3)),
                        TextWrapping = TextWrapping.Wrap, MaxLines = 3, TextTrimming = TextTrimming.CharacterEllipsis
                    });
                }
                catch (InvalidDataException ex)
                {
                    panel.Children.Add(new TextBlock { Text = ex.Message, TextWrapping = TextWrapping.Wrap });
                }
            }
            var follow = new ToggleSwitch { Header = "Live follow local reloads", IsOn = binding.LiveFollow };
            follow.Toggled += async (_, _) =>
            {
                if (_attached is not { } studio) return;
                follow.IsEnabled = false;
                try { _status.Text = (await studio.GraphicsPlayout.SetCrawlLiveFollowAsync(row.Id, follow.IsOn)).Reason; }
                catch (Exception ex) when (ex is not OutOfMemoryException) { _status.Text = ex.Message; }
                finally { follow.IsEnabled = true; }
            };
            panel.Children.Add(follow);
            if (_attached?.GraphicsPlayout.CrawlOnProgram(row.Id) is { } live)
            {
                var playing = live.ProgramFields?.AdditionalFields?.GetValueOrDefault(GraphicsCrawlData.RevisionField).ToString();
                var pending = live.CrawlPendingRevision > 0
                    ? live.Fields.AdditionalFields?.GetValueOrDefault(GraphicsCrawlData.RevisionField).ToString()
                    : null;
                panel.Children.Add(new TextBlock
                {
                    Text = $"Crawl {live.CrawlPlayback} · Program data {playing ?? "unknown"}" +
                        (pending is not null ? $" · pending {pending} ({live.Preparation})" : "") +
                        (live.Warning.Length > 0 ? $" · {live.Warning}" : ""),
                    TextWrapping = TextWrapping.Wrap
                });
                var buttons = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 6 };
                foreach (var (label, action) in new[] { ("Pause", "pause"), ("Resume", "resume"), ("Restart", "restart") })
                {
                    var button = StudioBrushes.GhostButton(label);
                    button.IsEnabled = action == "restart" || (action == "pause" ? live.CrawlPlayback == "playing" : live.CrawlPlayback == "paused");
                    button.Click += async (_, _) =>
                    {
                        if (_attached is not { } studio) return;
                        try { _status.Text = (await studio.GraphicsPlayout.ControlCrawlAsync(row.Id, action)).Reason; }
                        catch (Exception ex) when (ex is not OutOfMemoryException) { _status.Text = ex.Message; }
                    };
                    buttons.Children.Add(button);
                }
                panel.Children.Add(buttons);
            }
        }
        panel.Children.Add(reload);
        panel.Children.Add(publish);
        return panel;
    }
}
