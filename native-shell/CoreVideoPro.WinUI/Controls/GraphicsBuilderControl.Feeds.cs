using CoreVideoPro.MediaCore.Services;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;

namespace CoreVideoPro.WinUI.Controls;

public sealed partial class GraphicsBuilderControl
{
    private async Task ShowRemoteFeedsAsync()
    {
        if (_designBusy || _attached is not { } studio || XamlRoot is null) return;
        _designBusy = true;
        GraphicsDataSource? designSource = null;
        string? designKey = null;
        bool crawl = false;
        try
        {
            var library = studio.GraphicsLibrary;
            var feeds = studio.GraphicsFeeds;
            await library.LoadAsync();
            var list = new ComboBox { Header = "Saved feeds", DisplayMemberPath = nameof(GraphicsFeedDefinition.Name), HorizontalAlignment = HorizontalAlignment.Stretch };
            var name = new TextBox { Header = "Feed name", MaxLength = 128 };
            var format = new ComboBox
            {
                Header = "Format", ItemsSource = new[] { "Published Google Sheets CSV", "RSS 2.0", "HTTPS JSON" },
                SelectedIndex = 0, HorizontalAlignment = HorizontalAlignment.Stretch
            };
            var endpoint = new TextBox { Header = "HTTPS feed URL", MaxLength = 2048 };
            var key = new TextBox { Header = "Stable ID column", MaxLength = 128, Text = "id" };
            var interval = new NumberBox { Header = "Poll interval (seconds)", Minimum = 5, Maximum = 300, Value = 15 };
            var enabled = new ToggleSwitch { Header = "Enable polling" };
            var status = new TextBlock { TextWrapping = TextWrapping.Wrap };
            var summary = new TextBlock { TextWrapping = TextWrapping.Wrap, MaxLines = 3, TextTrimming = TextTrimming.CharacterEllipsis };
            var error = new TextBlock { TextWrapping = TextWrapping.Wrap };
            var add = StudioBrushes.GhostButton("New feed");
            var save = StudioBrushes.AccentButton("Save feed");
            var refresh = StudioBrushes.GhostButton("Refresh now");
            var tableButton = StudioBrushes.GhostButton("Create table…");
            var crawlButton = StudioBrushes.GhostButton("Create crawl…");
            var commands = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 8 };
            foreach (var button in new[] { add, save, refresh }) commands.Children.Add(button);
            var designs = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 8 };
            designs.Children.Add(tableButton);
            designs.Children.Add(crawlButton);
            Grid Pair(FrameworkElement first, FrameworkElement second)
            {
                var row = new Grid { ColumnSpacing = 8 };
                row.ColumnDefinitions.Add(new() { Width = new GridLength(1, GridUnitType.Star) });
                row.ColumnDefinitions.Add(new() { Width = new GridLength(1, GridUnitType.Star) });
                row.Children.Add(first);
                Grid.SetColumn(second, 1);
                row.Children.Add(second);
                return row;
            }
            var form = new StackPanel { Spacing = 8 };
            foreach (var control in new FrameworkElement[] { list, Pair(name, format), endpoint, Pair(key, interval), enabled, status, summary })
                form.Children.Add(control);
            var footer = new StackPanel { Spacing = 8 };
            footer.Children.Add(commands);
            footer.Children.Add(designs);
            footer.Children.Add(error);
            var panel = new Grid { RowSpacing = 8, MinWidth = 340, MaxHeight = 470 };
            panel.RowDefinitions.Add(new() { Height = new GridLength(1, GridUnitType.Star) });
            panel.RowDefinitions.Add(new() { Height = GridLength.Auto });
            panel.Children.Add(new ScrollViewer { Content = form, VerticalScrollBarVisibility = ScrollBarVisibility.Auto });
            Grid.SetRow(footer, 1);
            panel.Children.Add(footer);
            var dialog = new ContentDialog
            {
                Title = "Remote graphics feeds", XamlRoot = XamlRoot, RequestedTheme = ActualTheme,
                Content = panel, CloseButtonText = "Done"
            };

            GraphicsFeedDefinition? Selected() => list.SelectedItem as GraphicsFeedDefinition;
            GraphicsDataSource? Dataset() => library.Document.DataSources.FirstOrDefault(source => source.Id == Selected()?.Id);
            void RefreshDetails()
            {
                var feed = Selected();
                var data = Dataset();
                status.Text = feed is null ? "New feeds start with polling off." :
                    GraphicsFeedStatusText.Describe(feeds.Status(feed.Id), data);
                summary.Text = data is null ? "No accepted dataset yet." :
                    $"{data.Data.Rows.Length} rows · {data.Data.Columns.Length} columns\n" +
                    string.Join("  •  ", data.Data.Columns.Take(6).Select(column => $"{column.Id} ({column.Kind})"));
                refresh.IsEnabled = feed?.Enabled == true;
                tableButton.IsEnabled = data is not null;
                crawlButton.IsEnabled = data?.Data.Columns.Any(column => column.Kind == "text") == true;
            }
            void LoadSelection()
            {
                var feed = Selected();
                name.Text = feed?.Name ?? "";
                format.SelectedIndex = feed?.Format switch { "rss" => 1, "json" => 2, _ => 0 };
                endpoint.Text = feed?.Endpoint ?? "";
                key.Text = feed?.KeyColumn ?? "id";
                key.IsEnabled = format.SelectedIndex != 1;
                interval.Value = feed?.PollSeconds ?? 15;
                enabled.IsOn = feed?.Enabled == true;
                error.Text = "";
                RefreshDetails();
            }
            void LoadList(string? id)
            {
                var definitions = library.Document.Feeds.ToArray();
                list.ItemsSource = definitions;
                list.SelectedIndex = Array.FindIndex(definitions, feed => feed.Id == id);
                LoadSelection();
            }
            list.SelectionChanged += (_, _) => LoadSelection();
            format.SelectionChanged += (_, _) => key.IsEnabled = format.SelectedIndex != 1;
            add.Click += (_, _) => LoadList(null);
            refresh.Click += (_, _) =>
            {
                if (Selected() is { } feed) feeds.Refresh(feed.Id);
                RefreshDetails();
            };
            save.Click += async (_, _) =>
            {
                save.IsEnabled = false;
                try
                {
                    var current = Selected();
                    var provider = format.SelectedIndex switch { 1 => "rss", 2 => "json", _ => "sheets" };
                    var definition = new GraphicsFeedDefinition(current?.Id ?? "feed-" + Guid.NewGuid().ToString("N"),
                        name.Text.Trim(), provider, endpoint.Text.Trim(), provider == "rss" ? "id" : key.Text.Trim(),
                        enabled.IsOn, double.IsFinite(interval.Value) ? (int)Math.Round(interval.Value) : 15, current?.Generation ?? 0);
                    var saved = await feeds.SaveFeedAsync(definition);
                    if (saved is null) error.Text = library.Status;
                    else LoadList(saved.Id);
                }
                catch (Exception ex) when (ex is not OutOfMemoryException) { error.Text = "The feed settings could not be saved."; }
                finally { save.IsEnabled = true; }
            };
            void Bind(bool asCrawl)
            {
                if (Selected() is not { } feed || Dataset() is not { } data) return;
                designSource = data;
                designKey = feed.KeyColumn;
                crawl = asCrawl;
                dialog.Hide();
            }
            tableButton.Click += (_, _) => Bind(false);
            crawlButton.Click += (_, _) => Bind(true);
            EventHandler update = (_, _) => RefreshDetails();
            feeds.Changed += update;
            library.Changed += update;
            try
            {
                LoadList(library.Document.Feeds.FirstOrDefault()?.Id);
                await dialog.ShowAsync();
            }
            finally
            {
                feeds.Changed -= update;
                library.Changed -= update;
            }
        }
        catch (Exception ex) when (ex is not OutOfMemoryException) { _designStatus.Text = "Remote feed settings are unavailable."; }
        finally { _designBusy = false; }

        if (designSource is not null && _attached == studio)
        {
            if (crawl) await ShowCrawlDialogAsync(designSource, designKey);
            else await ShowDataTableDialogAsync(designSource, designKey);
        }
    }
}
