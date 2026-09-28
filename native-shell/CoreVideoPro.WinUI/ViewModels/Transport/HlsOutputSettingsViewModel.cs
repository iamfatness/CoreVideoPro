using CommunityToolkit.Mvvm.ComponentModel;
using CoreVideoPro.WinUI.Services;

namespace CoreVideoPro.WinUI.ViewModels.Transport;

public sealed class HlsOutputSettingsViewModel : ObservableObject
{
    private bool _enabled;
    private string _playlistUrl = string.Empty;

    public bool Enabled
    {
        get => _enabled;
        set
        {
            if (SetProperty(ref _enabled, value)) OnPropertyChanged(nameof(Summary));
        }
    }

    public string PlaylistUrl
    {
        get => _playlistUrl;
        set
        {
            if (SetProperty(ref _playlistUrl, value)) OnPropertyChanged(nameof(Summary));
        }
    }

    public string Summary => !Enabled ? "HLS disabled" :
        StudioStreamOutputValidation.ValidateHls(PlaylistUrl) is { } error ? error :
        $"HLS PUT ready - {PlaylistUrl.Trim()}";

    public string RedactedEndpoint => Uri.TryCreate(PlaylistUrl, UriKind.Absolute, out var uri) &&
        uri.Scheme is "http" or "https" && !string.IsNullOrWhiteSpace(uri.Host)
            ? $"{uri.Scheme}://{uri.Authority}/<hls-playlist>"
            : "<hls-playlist>";
}
