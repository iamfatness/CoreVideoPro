namespace CoreVideoPro.WinUI.Services;

public static class StudioStreamOutputValidation
{
    private static readonly HashSet<string> RtmpProtocols = new(StringComparer.OrdinalIgnoreCase)
    {
        "rtmp",
        "rtmps"
    };

    private static readonly HashSet<string> SrtModes = new(StringComparer.OrdinalIgnoreCase)
    {
        "caller",
        "listener",
        "rendezvous"
    };

    public static string NormalizeRtmpProtocol(string? protocol)
    {
        var normalized = NormalizeText(protocol, "rtmps").ToLowerInvariant();
        return RtmpProtocols.Contains(normalized) ? normalized : "rtmps";
    }

    public static string BuildRtmpUrl(string? protocol, string? serverUrl)
    {
        var normalizedUrl = NormalizeText(serverUrl, string.Empty);
        if (normalizedUrl.Length == 0 || normalizedUrl.Contains("://", StringComparison.Ordinal))
        {
            return normalizedUrl;
        }

        return $"{NormalizeRtmpProtocol(protocol)}://{normalizedUrl}";
    }

    public static string? ValidateRtmp(string? protocol, string? serverUrl, string? streamKey)
    {
        var normalizedProtocol = NormalizeText(protocol, "rtmps").ToLowerInvariant();
        if (!RtmpProtocols.Contains(normalizedProtocol))
        {
            return "Choose RTMP protocol rtmp or rtmps before streaming.";
        }

        var normalizedUrl = NormalizeText(serverUrl, string.Empty);
        if (normalizedUrl.Length == 0)
        {
            return "Configure RTMP server URL before streaming.";
        }

        var outputUrl = BuildRtmpUrl(normalizedProtocol, normalizedUrl);
        if (!Uri.TryCreate(outputUrl, UriKind.Absolute, out var uri) ||
            !RtmpProtocols.Contains(uri.Scheme))
        {
            return "RTMP server URL must be a valid rtmp:// or rtmps:// URL.";
        }

        if (string.IsNullOrWhiteSpace(uri.Host))
        {
            return "RTMP server URL must include a host.";
        }

        if (!string.Equals(uri.Scheme, normalizedProtocol, StringComparison.OrdinalIgnoreCase))
        {
            return "RTMP protocol selection must match the server URL scheme.";
        }

        if (string.IsNullOrWhiteSpace(streamKey))
        {
            return "Configure RTMP stream key before streaming.";
        }

        return null;
    }

    public static string NormalizeSrtMode(string? mode)
    {
        var normalized = NormalizeText(mode, "caller").ToLowerInvariant();
        return SrtModes.Contains(normalized) ? normalized : "caller";
    }

    public static int? ParseSrtKeyLength(string? keyLength) =>
        int.TryParse(keyLength, out var parsed) && parsed is 0 or 16 or 24 or 32
            ? parsed
            : null;

    public static string? ValidateSrt(
        string? mode,
        string? host,
        string? portText,
        string? latencyMsText,
        string? streamId,
        string? keyLengthText,
        string? passphrase)
    {
        var normalizedMode = NormalizeText(mode, "caller").ToLowerInvariant();
        if (!SrtModes.Contains(normalizedMode))
        {
            return "Choose SRT mode caller, listener, or rendezvous before streaming.";
        }

        if (string.IsNullOrWhiteSpace(host))
        {
            return "Configure SRT host before streaming.";
        }

        var port = ParsePositiveInt(portText);
        if (port is null or < 1 or > 65535)
        {
            return "Configure SRT port between 1 and 65535 before streaming.";
        }

        var latency = ParsePositiveInt(latencyMsText);
        if (latency is null or < 20)
        {
            return "Configure SRT latency of at least 20 ms before streaming.";
        }

        if (NormalizeText(streamId, string.Empty).Length > 512)
        {
            return "SRT stream ID must be 512 characters or less.";
        }

        var keyLength = ParseSrtKeyLength(keyLengthText);
        if (keyLength is null)
        {
            return "Choose SRT encryption key length 0, 16, 24, or 32.";
        }

        var normalizedPassphrase = NormalizeText(passphrase, string.Empty);
        if (keyLength > 0 && normalizedPassphrase.Length == 0)
        {
            return "Configure an SRT passphrase when encryption is enabled.";
        }

        if (keyLength > 0 && normalizedPassphrase.Length is < 10 or > 79)
        {
            return "SRT passphrase must be 10 to 79 characters when encryption is enabled.";
        }

        if (keyLength == 0 && normalizedPassphrase.Length > 0)
        {
            return "Choose an SRT key length before entering a passphrase.";
        }

        return null;
    }

    private static int? ParsePositiveInt(string? value) =>
        int.TryParse(value, out var parsed) ? parsed : null;

    private static string NormalizeText(string? value, string fallback) =>
        string.IsNullOrWhiteSpace(value) ? fallback : value.Trim();
}
