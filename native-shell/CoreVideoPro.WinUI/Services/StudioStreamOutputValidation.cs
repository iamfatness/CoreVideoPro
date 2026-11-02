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
        string? ffmpegBinDirectory = null,
        bool hlsEnabled = false,
        string? hlsPlaylistUrl = null)
    {
        if (!rtmpEnabled && !ndiEnabled && !srtEnabled && !hlsEnabled)
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

        if (hlsEnabled && ValidateHls(hlsPlaylistUrl) is { Length: > 0 } hlsError)
        {
            return hlsError;
        }

        if (hlsEnabled && ValidateFfmpegRuntime(ffmpegBinDirectory) is { Length: > 0 } hlsFfmpegError)
        {
            return hlsFfmpegError;
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

    public static string? ValidateHls(string? playlistUrl)
    {
        var value = playlistUrl?.Trim() ?? string.Empty;
        if (!Uri.TryCreate(value, UriKind.Absolute, out var uri) ||
            uri.Scheme is not ("http" or "https") || string.IsNullOrWhiteSpace(uri.Host))
        {
            return "Configure an http:// or https:// HLS playlist URL before streaming.";
        }

        if (uri.UserInfo.Length > 0 || uri.Query.Length > 0 || uri.Fragment.Length > 0 ||
            value.Any(char.IsWhiteSpace) || value.Contains('\\') || value.Contains('"') ||
            !uri.AbsolutePath.EndsWith(".m3u8", StringComparison.Ordinal))
        {
            return "HLS playlist URL must end in .m3u8 and contain no credentials, query, fragment, or whitespace.";
        }

        return null;
    }

    public static string? ValidateFfmpegRuntime(string? ffmpegBinDirectory, string? appDirectory = null)
    {
        return ResolveFfmpegExecutable(ffmpegBinDirectory, appDirectory) is not null
            ? null
            : string.IsNullOrWhiteSpace(ffmpegBinDirectory)
                ? "Configure FFmpeg before streaming RTMP/RTMPS. Choose the bin folder that contains ffmpeg.exe."
                : "FFmpeg folder not found or does not contain ffmpeg.exe. Choose the bin folder that contains ffmpeg.exe before streaming.";
    }

    public static string? ResolveFfmpegExecutable(string? ffmpegBinDirectory, string? appDirectory = null)
    {
        foreach (var candidate in FfmpegExecutableCandidates(ffmpegBinDirectory, appDirectory))
        {
            if (File.Exists(candidate))
            {
                return candidate;
            }
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

    /// <summary>NDI destination ids for the NDI output flag. The key is decided by the caller
    /// (<see cref="IncludeNdiGraphicsKeyDestination"/>) and rides with the program source only.</summary>
    public static IReadOnlyList<string> SelectNdiDestinations(
        bool ndiOn, string? programName, bool keyIncluded, bool validatedOnly)
    {
        var destinations = new List<string>(2);
        if (ndiOn && (!validatedOnly || CanSerializeNdiSettings(programName)))
        {
            destinations.Add("ndi");
            if (keyIncluded) destinations.Add(NdiGraphicsKeyDestination);
        }

        return destinations;
    }

    public static bool CanSerializeNdiSettings(string? programName) =>
        !string.IsNullOrWhiteSpace(programName);

    /// <summary>The core's destination id for the NDI graphics key (fill+key) sender.</summary>
    public const string NdiGraphicsKeyDestination = "ndi-graphics";

    /// <summary>NDI's source-name limit (native NdiSourceName.h kNdiMaxSourceNameLength).</summary>
    public const int NdiMaxSourceNameLength = 64;

    private const string NdiGraphicsKeySuffix = " Graphics";

    /// <summary>The NDI source name the graphics key advertises: the operator's own name when
    /// set, otherwise "&lt;NDI program name&gt; Graphics" so the two sources sort together on a
    /// receiver. The default trims the program part so the whole name fits NDI's 64 characters
    /// (an over-long default used to leave the sender stuck in source-name-invalid).</summary>
    public static string ResolveNdiGraphicsKeyName(string? programName, string? graphicsName)
    {
        var explicitName = NormalizeText(graphicsName, string.Empty);
        if (explicitName.Length > 0) return explicitName;
        var program = NormalizeText(programName, "CoreVideo Pro Program");
        var room = NdiMaxSourceNameLength - NdiGraphicsKeySuffix.Length;
        if (program.Length > room) program = program[..room].TrimEnd();
        return program + NdiGraphicsKeySuffix;
    }

    /// <summary>Validates the name the key would advertise, with the native sender's rules: at most
    /// 64 characters, none of NDI's reserved characters, and never the Program source's own name
    /// (two senders with one name collide on the network).</summary>
    public static string? ValidateNdiGraphicsKeyName(string? programName, string? graphicsName)
    {
        var resolved = ResolveNdiGraphicsKeyName(programName, graphicsName);
        if (resolved.Length > NdiMaxSourceNameLength)
            return $"NDI graphics key name must be {NdiMaxSourceNameLength} characters or fewer.";
        if (resolved.Any(ch => ch <= '\u001f' || "<>:\"/\\|?*".Contains(ch)))
            return "NDI graphics key name contains characters NDI does not allow (< > : \" / \\ | ? *).";
        if (string.Equals(resolved, NormalizeText(programName, "CoreVideo Pro Program"), StringComparison.OrdinalIgnoreCase))
            return "NDI graphics key name must differ from the NDI program name.";
        return null;
    }

    /// <summary>Why the graphics key cannot be turned on now, or null. One rule for the control
    /// action and the stream destination list. NDI must be on (the key rides with it), the running
    /// core must carry ndi-graphics-output (null = no profile yet: defer to the core's own sender
    /// warning), and the advertised name must be valid.</summary>
    public static string? NdiGraphicsKeyRefusal(bool ndiEnabled, bool? coreHasCapability, string? programName, string? graphicsName)
    {
        if (!ndiEnabled) return "Turn on NDI output first; the graphics key is a second NDI source that rides with it.";
        if (coreHasCapability == false)
            return "NDI graphics key is not available in this native core build (missing ndi-graphics-output).";
        return ValidateNdiGraphicsKeyName(programName, graphicsName);
    }

    /// <summary>Whether the key destination is sent. A key that cannot be sent is DROPPED from the
    /// list on its own; it never fails stream validation, so it can never stop RTMP, SRT or the
    /// Program NDI source.</summary>
    public static bool IncludeNdiGraphicsKeyDestination(bool ndiEnabled, bool keyEnabled, bool? coreHasCapability,
        string? programName, string? graphicsName) =>
        keyEnabled && NdiGraphicsKeyRefusal(ndiEnabled, coreHasCapability, programName, graphicsName) is null;

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

    private static IEnumerable<string> FfmpegExecutableCandidates(string? ffmpegBinDirectory, string? appDirectory)
    {
        if (!string.IsNullOrWhiteSpace(ffmpegBinDirectory))
        {
            yield return Path.Combine(ffmpegBinDirectory.Trim(), "ffmpeg.exe");
        }

        var envBin = Environment.GetEnvironmentVariable("COREVIDEO_FFMPEG_BIN_DIR");
        if (!string.IsNullOrWhiteSpace(envBin))
        {
            yield return Path.Combine(envBin.Trim(), "ffmpeg.exe");
        }

        var legacyEnvBin = Environment.GetEnvironmentVariable("FFMPEG_BIN_DIR");
        if (!string.IsNullOrWhiteSpace(legacyEnvBin))
        {
            yield return Path.Combine(legacyEnvBin.Trim(), "ffmpeg.exe");
        }

        var appRoot = string.IsNullOrWhiteSpace(appDirectory)
            ? AppContext.BaseDirectory
            : appDirectory.Trim();
        if (!string.IsNullOrWhiteSpace(appRoot))
        {
            yield return Path.Combine(appRoot, "ffmpeg.exe");
        }

        foreach (var executable in PathExecutableCandidates("ffmpeg.exe"))
        {
            yield return executable;
        }
    }

    private static IEnumerable<string> PathExecutableCandidates(string executableName)
    {
        var path = Environment.GetEnvironmentVariable("PATH");
        if (string.IsNullOrWhiteSpace(path))
        {
            yield break;
        }

        foreach (var entry in path.Split(Path.PathSeparator)
            .Where(entry => !string.IsNullOrWhiteSpace(entry))
            .Select(entry => Path.Combine(entry.Trim(), executableName)))
        {
            yield return entry;
        }
    }
}
