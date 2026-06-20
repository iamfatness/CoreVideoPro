using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Media.Imaging;
using Windows.Media.Core;
using Windows.Media.Playback;

namespace CoreVideoPro.WinUI.Controls;

public sealed partial class MediaAssetPreviewHost : UserControl
{
    public static readonly DependencyProperty FilePathProperty =
        DependencyProperty.Register(
            nameof(FilePath),
            typeof(string),
            typeof(MediaAssetPreviewHost),
            new PropertyMetadata(null, OnPreviewPropertyChanged));

    public static readonly DependencyProperty KindProperty =
        DependencyProperty.Register(
            nameof(Kind),
            typeof(string),
            typeof(MediaAssetPreviewHost),
            new PropertyMetadata(null, OnPreviewPropertyChanged));

    public static readonly DependencyProperty IsPlayingProperty =
        DependencyProperty.Register(
            nameof(IsPlaying),
            typeof(bool),
            typeof(MediaAssetPreviewHost),
            new PropertyMetadata(false, OnPreviewPropertyChanged));

    private MediaPlayer? _player;

    public MediaAssetPreviewHost()
    {
        InitializeComponent();
        Unloaded += OnUnloaded;
    }

    public string? FilePath
    {
        get => (string?)GetValue(FilePathProperty);
        set => SetValue(FilePathProperty, value);
    }

    public string? Kind
    {
        get => (string?)GetValue(KindProperty);
        set => SetValue(KindProperty, value);
    }

    public bool IsPlaying
    {
        get => (bool)GetValue(IsPlayingProperty);
        set => SetValue(IsPlayingProperty, value);
    }

    public string EmptyTitle =>
        string.IsNullOrWhiteSpace(FilePath) ? "No media selected" : "Preview unavailable";

    public string EmptyDetail =>
        string.IsNullOrWhiteSpace(FilePath)
            ? "Import and select a media asset to preview or cue it."
            : "The selected asset cannot be opened by the local preview host.";

    private static void OnPreviewPropertyChanged(DependencyObject sender, DependencyPropertyChangedEventArgs args)
    {
        if (sender is MediaAssetPreviewHost host)
        {
            host.RefreshPreview();
        }
    }

    private void RefreshPreview()
    {
        Bindings.Update();

        ImagePreview.Visibility = Visibility.Collapsed;
        VideoPreview.Visibility = Visibility.Collapsed;
        EmptyState.Visibility = Visibility.Visible;

        if (string.IsNullOrWhiteSpace(FilePath) || !File.Exists(FilePath))
        {
            StopPlayer();
            ImagePreview.Source = null;
            return;
        }

        var kind = Kind?.Trim().ToLowerInvariant() ?? string.Empty;
        if (kind == "image")
        {
            StopPlayer();
            ImagePreview.Source = new BitmapImage(new Uri(FilePath));
            ImagePreview.Visibility = Visibility.Visible;
            EmptyState.Visibility = Visibility.Collapsed;
            return;
        }

        if (kind is "video" or "audio")
        {
            _player ??= new MediaPlayer();
            VideoPreview.SetMediaPlayer(_player);
            _player.Source = MediaSource.CreateFromUri(new Uri(FilePath));
            VideoPreview.Visibility = Visibility.Visible;
            EmptyState.Visibility = Visibility.Collapsed;

            if (IsPlaying)
            {
                _player.Play();
            }
            else
            {
                _player.Pause();
            }
        }
    }

    private void StopPlayer()
    {
        if (_player is null)
        {
            return;
        }

        _player.Pause();
        _player.Source = null;
    }

    private void OnUnloaded(object sender, RoutedEventArgs e)
    {
        StopPlayer();
        _player?.Dispose();
        _player = null;
    }
}
