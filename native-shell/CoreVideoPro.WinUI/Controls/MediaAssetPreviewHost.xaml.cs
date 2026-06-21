using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Media;
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

    public static readonly DependencyProperty PlaybackKeyProperty =
        DependencyProperty.Register(
            nameof(PlaybackKey),
            typeof(string),
            typeof(MediaAssetPreviewHost),
            new PropertyMetadata(null, OnPreviewPropertyChanged));

    public static readonly DependencyProperty PreviewStretchProperty =
        DependencyProperty.Register(
            nameof(PreviewStretch),
            typeof(Stretch),
            typeof(MediaAssetPreviewHost),
            new PropertyMetadata(Stretch.Uniform, OnPreviewPropertyChanged));

    private static readonly HashSet<string> CompletedPlaybackKeys = new(StringComparer.Ordinal);
    private static readonly object CompletedPlaybackGate = new();

    private MediaPlayer? _player;
    private string? _loadedFilePath;
    private string? _loadedMediaType;
    private string? _loadedImagePath;
    private string? _loadedPlaybackKey;
    private bool _playbackCompleted;

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

    public string? PlaybackKey
    {
        get => (string?)GetValue(PlaybackKeyProperty);
        set => SetValue(PlaybackKeyProperty, value);
    }

    public Stretch PreviewStretch
    {
        get => (Stretch)GetValue(PreviewStretchProperty);
        set => SetValue(PreviewStretchProperty, value);
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
            _loadedImagePath = null;
            return;
        }

        var mediaType = ResolveMediaType(FilePath, Kind);
        if (mediaType == "image")
        {
            StopPlayer();
            if (!string.Equals(_loadedImagePath, FilePath, StringComparison.OrdinalIgnoreCase))
            {
                ImagePreview.Source = new BitmapImage(new Uri(FilePath));
                _loadedImagePath = FilePath;
            }

            ImagePreview.Visibility = Visibility.Visible;
            EmptyState.Visibility = Visibility.Collapsed;
            return;
        }

        if (mediaType is "video" or "audio")
        {
            _loadedImagePath = null;
            ImagePreview.Source = null;
            _player ??= new MediaPlayer();
            _player.IsLoopingEnabled = false;
            _player.MediaEnded -= OnMediaEnded;
            _player.MediaEnded += OnMediaEnded;
            VideoPreview.SetMediaPlayer(_player);
            var playbackKey = ResolvePlaybackKey(FilePath);
            if (!string.Equals(_loadedFilePath, FilePath, StringComparison.OrdinalIgnoreCase) ||
                !string.Equals(_loadedMediaType, mediaType, StringComparison.Ordinal) ||
                !string.Equals(_loadedPlaybackKey, playbackKey, StringComparison.Ordinal))
            {
                _player.Source = MediaSource.CreateFromUri(new Uri(FilePath));
                _loadedFilePath = FilePath;
                _loadedMediaType = mediaType;
                _loadedPlaybackKey = playbackKey;
                _playbackCompleted = IsPlaybackKeyCompleted(playbackKey);
            }

            VideoPreview.Visibility = Visibility.Visible;
            EmptyState.Visibility = Visibility.Collapsed;

            if (IsPlaying && !_playbackCompleted)
            {
                _player.Play();
            }
            else
            {
                if (!IsPlaying)
                {
                    ClearCompletedPlaybackKey(playbackKey);
                    _playbackCompleted = false;
                    _player.PlaybackSession.Position = TimeSpan.Zero;
                }

                _player.Pause();
            }
        }
    }

    private void OnMediaEnded(MediaPlayer sender, object args)
    {
        _playbackCompleted = true;
        if (_loadedPlaybackKey is { Length: > 0 } playbackKey)
        {
            MarkPlaybackKeyCompleted(playbackKey);
        }

        DispatcherQueue.TryEnqueue(() =>
        {
            sender.Pause();
            Bindings.Update();
        });
    }

    private static string ResolveMediaType(string filePath, string? kind)
    {
        var extension = Path.GetExtension(filePath).ToLowerInvariant();
        if (extension is ".png" or ".jpg" or ".jpeg" or ".gif")
        {
            return "image";
        }

        if (extension is ".mp4" or ".mov" or ".webm")
        {
            return "video";
        }

        if (extension is ".wav" or ".mp3" or ".aac" or ".m4a")
        {
            return "audio";
        }

        return kind?.Trim().ToLowerInvariant() switch
        {
            "lower-third" => "image",
            "stinger" or "slate" => "video",
            "audio-bed" => "audio",
            "image" or "video" or "audio" => kind.Trim().ToLowerInvariant(),
            _ => string.Empty
        };
    }

    private string ResolvePlaybackKey(string filePath) =>
        string.IsNullOrWhiteSpace(PlaybackKey) ? filePath : PlaybackKey.Trim();

    private static bool IsPlaybackKeyCompleted(string playbackKey)
    {
        lock (CompletedPlaybackGate)
        {
            return CompletedPlaybackKeys.Contains(playbackKey);
        }
    }

    private static void MarkPlaybackKeyCompleted(string playbackKey)
    {
        lock (CompletedPlaybackGate)
        {
            CompletedPlaybackKeys.Add(playbackKey);
        }
    }

    private static void ClearCompletedPlaybackKey(string playbackKey)
    {
        lock (CompletedPlaybackGate)
        {
            CompletedPlaybackKeys.Remove(playbackKey);
        }
    }

    private void StopPlayer()
    {
        if (_player is null)
        {
            _loadedFilePath = null;
            _loadedMediaType = null;
            _loadedPlaybackKey = null;
            return;
        }

        _player.Pause();
        _player.Source = null;
        _loadedFilePath = null;
        _loadedMediaType = null;
        _loadedPlaybackKey = null;
        _playbackCompleted = false;
    }

    private void OnUnloaded(object sender, RoutedEventArgs e)
    {
        StopPlayer();
        _player?.Dispose();
        _player = null;
    }
}
