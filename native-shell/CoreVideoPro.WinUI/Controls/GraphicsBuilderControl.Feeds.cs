using CoreVideoPro.MediaCore.Services;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;

namespace CoreVideoPro.WinUI.Controls;

public sealed partial class GraphicsBuilderControl
{
    private static readonly string[] FeedFormats = ["sheets", "rss", "json", "custom-json", "wss"];

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
                Header = "Format", ItemsSource = new[] { "Published Google Sheets CSV", "RSS 2.0", "HTTPS JSON", "Custom API (pull)", "Custom API (push)" },
                SelectedIndex = 0, HorizontalAlignment = HorizontalAlignment.Stretch
            };
            var endpoint = new TextBox { Header = "HTTPS feed URL", MaxLength = 2048 };
            var key = new TextBox { Header = "Stable ID column", MaxLength = 128, Text = "id" };
            var interval = new NumberBox { Header = "Poll interval (seconds)", Minimum = 5, Maximum = 300, Value = 15 };
            var enabled = new ToggleSwitch { Header = "Enable polling" };
            var rowsPointer = new TextBox { Header = "Rows JSON Pointer", PlaceholderText = "/data/results (blank for root array)", MaxLength = 512 };
            var columnMappings = new TextBox { Header = "Column mappings", PlaceholderText = "One per line, e.g. team = /team/name",
                AcceptsReturn = true, TextWrapping = TextWrapping.Wrap, MaxHeight = 108, MaxLength = 21_000 };
            var bearer = new PasswordBox { Header = "New bearer token (optional)", MaxLength = 4096 };
            var removeBearer = new CheckBox { Content = "Remove saved token" };
            var credential = new TextBlock { TextWrapping = TextWrapping.Wrap };
            var connectionHelp = new TextBlock { TextWrapping = TextWrapping.Wrap,
                Text = "Connects to a WSS producer that sends numbered, complete CoreVideo snapshots." };
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
            var mappings = new StackPanel { Spacing = 6, Children = { rowsPointer, columnMappings } };
            var credentials = new StackPanel { Spacing = 4, Children = { bearer, removeBearer } };
            foreach (var control in new FrameworkElement[] { list, Pair(name, format), endpoint, Pair(key, interval), enabled,
                         connectionHelp, mappings, credentials, status })
                form.Children.Add(control);
            var footer = new StackPanel { Spacing = 8 };
            footer.Children.Add(credential);
            footer.Children.Add(summary);
            footer.Children.Add(commands);
            footer.Children.Add(designs);
            footer.Children.Add(error);
            var panel = new Grid { RowSpacing = 8, MinWidth = 340, MaxHeight = 500 };
            panel.RowDefinitions.Add(new() { Height = new GridLength(1, GridUnitType.Star) });
            panel.RowDefinitions.Add(new() { Height = GridLength.Auto });
            panel.Children.Add(new ScrollViewer { Name = "RemoteFeedFields", Content = form,
                VerticalScrollBarVisibility = ScrollBarVisibility.Visible });
            Grid.SetRow(footer, 1);
            panel.Children.Add(footer);
            var dialog = new ContentDialog
            {
                Title = "Remote graphics feeds", XamlRoot = XamlRoot, RequestedTheme = ActualTheme,
                Content = panel, CloseButtonText = "Done"
            };

            GraphicsFeedDefinition? Selected() => list.SelectedItem as GraphicsFeedDefinition;
            GraphicsDataSource? Dataset() => library.Document.DataSources.FirstOrDefault(source => source.Id == Selected()?.Id);
            string Provider() => format.SelectedIndex >= 0 ? FeedFormats[format.SelectedIndex] : FeedFormats[0];
            bool localCredential = false;
            bool checkingCredential = false;
            int selectionVersion = 0;
            void RefreshDetails()
            {
                var feed = Selected();
                var data = Dataset();
                var push = Provider() == "wss";
                var pull = Provider() == "custom-json";
                var custom = push || pull;
                endpoint.Header = push ? "WSS feed URL" : "HTTPS feed URL";
                key.IsEnabled = Provider() != "rss";
                interval.Visibility = push ? Visibility.Collapsed : Visibility.Visible;
                mappings.Visibility = pull ? Visibility.Visible : Visibility.Collapsed;
                credentials.Visibility = custom ? Visibility.Visible : Visibility.Collapsed;
                credential.Visibility = credentials.Visibility;
                connectionHelp.Visibility = push ? Visibility.Visible : Visibility.Collapsed;
                enabled.Header = push ? "Enable connection" : "Enable polling";
                refresh.Content = push ? "Reconnect now" : "Refresh now";
                var hasBinding = feed?.CredentialReference is not null && feed.Endpoint == endpoint.Text.Trim() && feed.Format == Provider();
                credential.Text = hasBinding ? checkingCredential ? "Checking local credential…" : localCredential ?
                    "Credential stored on this computer" : "Authorize this feed on this computer" : "Credential status · Anonymous";
                removeBearer.Visibility = hasBinding ? Visibility.Visible : Visibility.Collapsed;
                status.Text = feed is null ? "New feeds start disabled." :
                    GraphicsFeedStatusText.Describe(feeds.Status(feed.Id), data, feed.Format == "wss");
                summary.Text = data is null ? "No accepted dataset yet." :
                    $"{data.Data.Rows.Length} rows · {data.Data.Columns.Length} columns\n" +
                    string.Join("  •  ", data.Data.Columns.Take(6).Select(column => $"{column.Id} ({column.Kind})"));
                if (pull)
                    summary.Text = data is null ? "Mapping preview · Save and refresh the feed to see values." :
                        "Mapping preview · accepted revision " + data.Revision + "\n" +
                        string.Join("\n", data.Data.Rows.Take(2).Select(row =>
                            string.Join("  •  ", row.Cells.Take(4).Select(cell => cell.Length > 48 ? cell[..48] + "…" : cell))));
                refresh.IsEnabled = feed?.Enabled == true && feed.Format == Provider() && feed.Endpoint == endpoint.Text.Trim();
                tableButton.IsEnabled = data is not null;
                crawlButton.IsEnabled = data?.Data.Columns.Any(column => column.Kind == "text") == true;
            }
            void LoadSelection()
            {
                var feed = Selected();
                var version = ++selectionVersion;
                localCredential = false;
                checkingCredential = feed?.CredentialReference is not null;
                name.Text = feed?.Name ?? "";
                format.SelectedIndex = Math.Max(0, Array.IndexOf(FeedFormats, feed?.Format));
                endpoint.Text = feed?.Endpoint ?? "";
                key.Text = feed?.KeyColumn ?? "id";
                key.IsEnabled = format.SelectedIndex != 1;
                interval.Value = feed?.PollSeconds ?? 15;
                enabled.IsOn = feed?.Enabled == true;
                rowsPointer.Text = feed?.Mapping?.RowsPointer ?? "";
                columnMappings.Text = feed?.Mapping is { } mapping ?
                    string.Join("\n", mapping.Columns.Select(column => $"{column.Column} = {column.Pointer}")) : "";
                bearer.Password = "";
                removeBearer.IsChecked = false;
                error.Text = "";
                RefreshDetails();
                if (checkingCredential) _ = CheckCredentialAsync(feed!, version);
            }
            async Task CheckCredentialAsync(GraphicsFeedDefinition feed, int version)
            {
                var available = await feeds.HasLocalCredentialAsync(feed);
                if (selectionVersion != version) return;
                localCredential = available;
                checkingCredential = false;
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
            format.SelectionChanged += (_, _) =>
            {
                bearer.Password = "";
                removeBearer.IsChecked = false;
                RefreshDetails();
            };
            endpoint.TextChanged += (_, _) =>
            {
                bearer.Password = "";
                removeBearer.IsChecked = false;
                RefreshDetails();
            };
            removeBearer.Checked += (_, _) =>
            {
                bearer.Password = "";
                bearer.IsEnabled = false;
            };
            removeBearer.Unchecked += (_, _) => bearer.IsEnabled = true;
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
                    var provider = Provider();
                    var custom = provider is "wss" or "custom-json";
                    var definition = new GraphicsFeedDefinition(current?.Id ?? "feed-" + Guid.NewGuid().ToString("N"),
                        name.Text.Trim(), provider, endpoint.Text.Trim(), provider == "rss" ? "id" : key.Text.Trim(),
                        enabled.IsOn, double.IsFinite(interval.Value) ? (int)Math.Round(interval.Value) : 15, current?.Generation ?? 0)
                    {
                        Mapping = provider == "custom-json" ? ParseMapping(rowsPointer.Text, columnMappings.Text) : null,
                        CredentialReference = custom ? current?.CredentialReference : null
                    };
                    var saved = await feeds.SaveFeedAsync(definition,
                        custom && bearer.Password.Length > 0 ? bearer.Password : null, removeBearer.IsChecked == true);
                    if (saved is null) error.Text = library.Status;
                    else LoadList(saved.Id);
                }
                catch (InvalidDataException ex) { error.Text = ex.Message; }
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
                selectionVersion++;
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

    private static GraphicsFeedMapping? ParseMapping(string rowsPointer, string text)
    {
        var columns = new List<GraphicsFeedColumnMapping>();
        foreach (var line in text.Split('\n', StringSplitOptions.TrimEntries | StringSplitOptions.RemoveEmptyEntries))
        {
            var equals = line.IndexOf('=');
            if (equals <= 0) throw new InvalidDataException("Enter one column mapping per line as column = /pointer.");
            columns.Add(new(line[..equals].Trim(), line[(equals + 1)..].Trim()));
        }
        var root = rowsPointer.Trim();
        return root.Length == 0 && columns.Count == 0 ? null : new(root, columns.ToArray());
    }
}
