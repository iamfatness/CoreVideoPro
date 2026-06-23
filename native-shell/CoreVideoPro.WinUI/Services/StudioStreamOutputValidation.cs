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

    public static string? ValidateSelectedDestinations(
        bool rtmpEnabled,
        bool ndiEnabled,
        bool srtEnabled,
        string? rtmpProtocol,
        string? rtmpServerUrl,
        string? rtmpStreamKey,
        string? ndiProgramName,
        string? srtMode,
        string? srtHost,
        string? srtPort,
        string? srtLatencyMs,
        string? srtStreamId,
        string? srtKeyLength,
        string? srtPassphrase,
        string? ffmpegBinDirectory = null)
    {
        if (!rtmpEnabled && !ndiEnabled && !srtEnabled)
        {
            return "Select at least one stream destination.";
        }

        if (rtmpEnabled && ValidateRtmp(rtmpProtocol, rtmpServerUrl, rtmpStreamKey) is { Length: > 0 } rtmpError)
        {
            return rtmpError;
        }

        if (rtmpEnabled && ValidateFfmpegRuntime(ffmpegBinDirectory) is { Length: > 0 } ffmpegError)
        {
            return ffmpegError;
        }

        if (ndiEnabled && string.IsNullOrWhiteSpace(ndiProgramName))
        {
            return "Configure an NDI program name before streaming.";
        }

        if (srtEnabled &&
            ValidateSrt(srtMode, srtHost, srtPort, srtLatencyMs, srtStreamId, srtKeyLength, srtPassphrase) is { Length: > 0 } srtError)
        {
            return srtError;
        }

        return null;
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

        if (string.IsNullOrWhiteSpace(uri.AbsolutePath) ||
            uri.AbsolutePath.Trim('/').Length == 0)
        {
            return "RTMP server URL must include an application path.";
        }

        var normalizedStreamKey = NormalizeText(streamKey, string.Empty);
        if (normalizedStreamKey.Length == 0)
        {
            return "Configure RTMP stream key before streaming.";
        }

        if (normalizedStreamKey.Any(char.IsWhiteSpace))
        {
            return "RTMP stream key cannot contain whitespace.";
        }

        return null;
    }

    public static bool CanSerializeRtmpSettings(string? protocol, string? serverUrl, string? streamKey) =>
        ValidateRtmp(protocol, serverUrl, streamKey) is null;

    public static string? ValidateFfmpegRuntime(string? ffmpegBinDirectory)
    {
        if (!string.IsNullOrWhiteSpace(ffmpegBinDirectory))
        {
            var normalized = ffmpegBinDirectory.Trim();
            if (!Directory.Exists(normalized))
            {
                return "FFmpeg folder not found. Choose the bin folder that contains ffmpeg.exe before streaming.";
            }

            return File.Exists(Path.Combine(normalized, "ffmpeg.exe"))
                ? null
                : "FFmpeg folder must contain ffmpeg.exe before streaming.";
        }

        return PathHasExecutable("ffmpeg.exe")
            ? null
            : "Configure FFmpeg before streaming RTMP/RTMPS. Choose the bin folder that contains ffmpeg.exe.";
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

    public static bool CanSerializeNdiSettings(string? programName) =>
        !string.IsNullOrWhiteSpace(programName);

    public static bool CanSerializeSrtSettings(
        string? mode,
        string? host,
        string? portText,
        string? latencyMsText,
        string? streamId,
        string? keyLengthText,
        string? passphrase) =>
        ValidateSrt(mode, host, portText, latencyMsText, streamId, keyLengthText, passphrase) is null;

    private static int? ParsePositiveInt(string? value) =>
        int.TryParse(value, out var parsed) ? parsed : null;

    private static string NormalizeText(string? value, string fallback) =>
        string.IsNullOrWhiteSpace(value) ? fallback : value.Trim();

    private static bool PathHasExecutable(string executableName)
    {
        var path = Environment.GetEnvironmentVariable("PATH");
        if (string.IsNullOrWhiteSpace(path))
        {
            return false;
        }

        return path.Split(Path.PathSeparator)
            .Where(entry => !string.IsNullOrWhiteSpace(entry))
            .Any(entry => File.Exists(Path.Combine(entry.Trim(), executableName)));
    }
}
