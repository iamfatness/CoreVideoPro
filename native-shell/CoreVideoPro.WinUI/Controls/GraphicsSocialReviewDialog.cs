using System.Security.Cryptography;
using CoreVideoPro.MediaCore.Services;
using CoreVideoPro.WinUI.Services;
using CoreVideoPro.WinUI.ViewModels;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;

namespace CoreVideoPro.WinUI.Controls;

internal sealed class GraphicsSocialReviewDialog : ContentDialog
{
    private readonly GraphicsSocialReviewViewModel _model;
    private readonly TextBox _video = new() { Header = "Public YouTube video ID", MaxLength = 11 };
    private readonly PasswordBox _key = new() { Header = "YouTube Data API key", PlaceholderText = "Leave blank to reuse the local key", MaxLength = 128 };
    private readonly ListView _messages = new() { Height = 115, SelectionMode = ListViewSelectionMode.Single };
    private readonly TextBlock _status = Note("");
    private readonly TextBlock _content = new() { TextWrapping = TextWrapping.Wrap };
    private readonly Button _connect = StudioBrushes.GhostButton("Connect");
    private readonly Button _disconnect = StudioBrushes.GhostButton("Disconnect");
    private readonly Button _clear = StudioBrushes.GhostButton("Clear local data");
    private readonly Button _approve = StudioBrushes.GhostButton("Approve");
    private readonly Button _reject = StudioBrushes.GhostButton("Reject");
    private readonly Button _withdraw = StudioBrushes.GhostButton("Withdraw");
    private readonly Button _show = StudioBrushes.GhostButton("Show approved");
    private readonly Button _hide = StudioBrushes.GhostButton("Hide social graphic");
    private IReadOnlyList<GraphicsSocialItem> _items = [];
    private GraphicsSocialItem? _selected;
    private bool _refreshing, _busy, _disconnecting;

    internal GraphicsSocialReviewDialog(XamlRoot root, GraphicsSocialReviewViewModel model)
    {
        _model = model;
        XamlRoot = root;
        Title = "Social review · YouTube live chat";
        CloseButtonText = "Close";
        _video.Text = model.VideoId;
        var content = new StackPanel { Spacing = 8 };
        content.Children.Add(Note("Review each message before approving it. Show approved is a separate action."));
        var source = new Grid { ColumnSpacing = 8 };
        source.ColumnDefinitions.Add(new() { Width = new GridLength(1, GridUnitType.Star) });
        source.ColumnDefinitions.Add(new() { Width = new GridLength(2, GridUnitType.Star) });
        source.Children.Add(_video);
        Grid.SetColumn(_key, 1);
        source.Children.Add(_key);
        content.Children.Add(source);
        content.Children.Add(Row(_connect, _disconnect, _clear));
        content.Children.Add(_status);
        content.Children.Add(_messages);
        content.Children.Add(new ScrollViewer { MinHeight = 42, MaxHeight = 85, Content = _content,
            HorizontalScrollBarVisibility = ScrollBarVisibility.Disabled, VerticalScrollBarVisibility = ScrollBarVisibility.Auto });
        content.Children.Add(Row(_approve, _reject, _withdraw));
        content.Children.Add(Row(_show, _hide));
        Content = new ScrollViewer { MaxHeight = 485, Content = content, VerticalScrollBarVisibility = ScrollBarVisibility.Auto,
            HorizontalScrollBarVisibility = ScrollBarVisibility.Disabled };

        _messages.SelectionChanged += (_, _) =>
        {
            if (_refreshing) return;
            _selected = (_messages.SelectedItem as ListViewItem)?.Tag as GraphicsSocialItem;
            RefreshSelection();
        };
        _video.TextChanged += (_, _) => RefreshSelection();
        _connect.Click += async (_, _) => await RunAsync(async () =>
        {
            var key = _key.Password.Trim();
            _key.Password = "";
            await _model.ConnectAsync(_video.Text.Trim(), key);
        });
        _disconnect.Click += async (_, _) => await DisconnectAsync();
        _clear.Click += async (_, _) => await RunAsync(async () =>
        {
            await _model.ClearAsync();
            _key.Password = "";
            _video.Text = "";
        });
        _approve.Click += (_, _) => { if (_selected is { } item) _model.Decide(item.Key, approve: true); };
        _reject.Click += (_, _) => { if (_selected is { } item) _model.Decide(item.Key, approve: false); };
        _withdraw.Click += async (_, _) => { if (_selected is { } item) await RunAsync(() => _model.WithdrawAsync(item.Key)); };
        _show.Click += async (_, _) => { if (_selected is { } item) await RunAsync(() => _model.ShowAsync(item.Key)); };
        _hide.Click += async (_, _) => await RunAsync(_model.HideAsync);
        Opened += (_, _) => { _model.Changed += OnChanged; Refresh(); };
        Closed += (_, _) => { _model.Changed -= OnChanged; _key.Password = ""; };
        Refresh();
    }

    internal static GraphicsSocialItem? PreserveSelection(IReadOnlyList<GraphicsSocialItem> items, GraphicsSocialItemKey? key) =>
        key is { } selected ? items.FirstOrDefault(item => item.Key == selected) : null;

    private static TextBlock Note(string text) => new() { Text = text, TextWrapping = TextWrapping.Wrap, Foreground = StudioBrushes.Muted };
    private static StackPanel Row(params Button[] buttons)
    {
        var row = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 8 };
        foreach (var button in buttons) row.Children.Add(button);
        return row;
    }

    private void OnChanged(object? sender, EventArgs args) => Refresh();

    private void Refresh()
    {
        var items = _model.Playout.Items;
        if (!_items.SequenceEqual(items))
        {
            // Rebinding a changed revision clears selection. An old click must not
            // silently approve the replacement version before it is read.
            _selected = PreserveSelection(items, _selected?.Key);
            _refreshing = true;
            try
            {
                _messages.Items.Clear();
                foreach (var item in items)
                {
                    var entry = new ListViewItem { Content = $"{item.Author} · {item.Decision} · {item.ReceivedAt.ToLocalTime():t}", Tag = item };
                    _messages.Items.Add(entry);
                    if (item.Key == _selected?.Key) _messages.SelectedItem = entry;
                }
            }
            finally { _refreshing = false; }
            _items = items;
        }
        _status.Text = _model.Playout.OnProgram ? "On Program · " + _model.Status : _model.Status;
        RefreshSelection();
    }

    private void RefreshSelection()
    {
        _content.Text = _selected is { } item ? item.Author + Environment.NewLine + item.Message : "Select one message to read it.";
        var active = !_busy && !_disconnecting && _model.Playout.IsConnected;
        _approve.IsEnabled = _reject.IsEnabled = active && _selected?.Decision == GraphicsSocialDecision.Pending;
        _show.IsEnabled = active && _selected?.Decision == GraphicsSocialDecision.Approved;
        _withdraw.IsEnabled = active && _selected is not null;
        _hide.IsEnabled = !_busy && _model.Playout.OnProgram;
        _connect.IsEnabled = !_busy && !_disconnecting && YouTubeSocialFeed.IsVideoId(_video.Text.Trim());
        _clear.IsEnabled = !_busy && !_disconnecting;
        _disconnect.IsEnabled = !_disconnecting && (_model.Running || _model.Playout.IsConnected);
    }

    private async Task RunAsync(Func<Task> action)
    {
        if (_busy || _disconnecting) return;
        _busy = true;
        RefreshSelection();
        try { await action(); }
        catch (Exception error) when (error is IOException or UnauthorizedAccessException or CryptographicException or InvalidOperationException or OperationCanceledException)
        { _status.Text = "Social review could not complete that action. Check the connection and retry."; }
        finally { _busy = false; RefreshSelection(); }
    }

    private async Task DisconnectAsync()
    {
        if (_disconnecting) return;
        _disconnecting = true;
        RefreshSelection();
        try { await _model.DisconnectAsync(); }
        catch (Exception error) when (error is IOException or InvalidOperationException or OperationCanceledException)
        { _status.Text = "Social review stopped. Check the Program strip for any remaining card."; }
        finally { _disconnecting = false; RefreshSelection(); }
    }
}
