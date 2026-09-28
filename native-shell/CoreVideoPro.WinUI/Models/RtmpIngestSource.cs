using CommunityToolkit.Mvvm.ComponentModel;

namespace CoreVideoPro.WinUI.Models;

public sealed class RtmpIngestSource : ObservableObject
{
    public required string Id { get; init; }
    public required int Number { get; init; }

    private string _url = string.Empty;
    public string Url
    {
        get => _url;
        set
        {
            if (SetProperty(ref _url, value))
            {
                OnPropertyChanged(nameof(Summary));
            }
        }
    }

    public string DeviceId => $"rtmp-ingest-{Number:00}";
    public string Name => $"RTMP {Number}";

    // The stream path can act as a secret. Never project it into device names,
    // status text, capture logs, or the unified source picker.
    public string Summary => RtmpIngestSourcePolicy.IsValid(Url)
        ? "Listener configured — publish to the URL below"
        : "Enter rtmp://host:port/app/stream to enable this listener";
}

public static class RtmpIngestSourcePolicy
{
    public static bool IsValid(string? value)
    {
        if (string.IsNullOrWhiteSpace(value)) return false;
        var url = value.Trim();
        if (!url.StartsWith("rtmp://", StringComparison.Ordinal) ||
            url.Any(char.IsWhiteSpace) || url.IndexOfAny(['?', '#', '"', '\\']) >= 0)
            return false;
        var authorityStart = "rtmp://".Length;
        var pathStart = url.IndexOf('/', authorityStart);
        if (pathStart <= authorityStart || pathStart == url.Length - 1 ||
            url.LastIndexOf('/') == pathStart || url.EndsWith('/')) return false;
        var authority = url[authorityStart..pathStart];
        return authority.Length > 0 && authority[0] != ':' && !authority.Contains('@') &&
            Uri.TryCreate(url, UriKind.Absolute, out var parsed) &&
            parsed.Host.Length > 0 && parsed.Port != 0;
    }

    public static int ListenerPort(string url)
    {
        var port = new Uri(url).Port;
        return port < 0 ? 1935 : port;
    }
}
