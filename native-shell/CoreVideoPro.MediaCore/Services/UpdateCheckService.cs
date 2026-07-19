using System.Text.Json;

namespace CoreVideoPro.MediaCore.Services;

/// <summary>
/// One entry of the release feed (`latest.json`) published next to the
/// `.appinstaller` by the release pipeline (scripts/make-appinstaller.mjs
/// --latest-json). Shape:
/// {"version":"x.y.z","msixUrl":"...","appinstallerUrl":"...","sha256":"..."}
/// </summary>
public sealed record UpdateFeedEntry(
    string Version,
    string? MsixUrl,
    string? AppInstallerUrl,
    string? Sha256)
{
    /// <summary>The URL an operator should open to get the update.</summary>
    public string? DownloadUrl => string.IsNullOrWhiteSpace(AppInstallerUrl) ? MsixUrl : AppInstallerUrl;
}

/// <summary>
/// Real version parsing/comparison for update decisions — never a string
/// compare ("1.10.0" is newer than "1.9.9"). Accepts "x.y.z", an optional
/// leading "v", and an optional fourth part (MSIX "x.y.z.0" form).
/// </summary>
public static class AppUpdateVersion
{
    public static bool TryParse(string? text, out Version version)
    {
        version = new Version(0, 0, 0, 0);
        if (string.IsNullOrWhiteSpace(text))
        {
            return false;
        }

        var trimmed = text.Trim();
        if (trimmed.StartsWith('v') || trimmed.StartsWith('V'))
        {
            trimmed = trimmed[1..];
        }

        var parts = trimmed.Split('.');
        if (parts.Length is < 2 or > 4)
        {
            return false;
        }

        Span<int> numbers = stackalloc int[4];
        for (var i = 0; i < parts.Length; i++)
        {
            if (!int.TryParse(parts[i], System.Globalization.NumberStyles.None,
                    System.Globalization.CultureInfo.InvariantCulture, out var value) || value < 0)
            {
                return false;
            }

            numbers[i] = value;
        }

        version = new Version(numbers[0], numbers[1], numbers[2], numbers[3]);
        return true;
    }

    public static bool IsNewer(string? candidate, string? current) =>
        TryParse(candidate, out var candidateVersion) &&
        TryParse(current, out var currentVersion) &&
        candidateVersion > currentVersion;

    public static bool AreEqual(string? left, string? right) =>
        TryParse(left, out var leftVersion) &&
        TryParse(right, out var rightVersion) &&
        leftVersion == rightVersion;
}

/// <summary>Parses `latest.json`; malformed input yields null, never throws.</summary>
public static class UpdateFeedParser
{
    public static UpdateFeedEntry? TryParse(string? json)
    {
        if (string.IsNullOrWhiteSpace(json))
        {
            return null;
        }

        try
        {
            using var document = JsonDocument.Parse(json);
            if (document.RootElement.ValueKind != JsonValueKind.Object)
            {
                return null;
            }

            var version = ReadString(document.RootElement, "version");
            if (!AppUpdateVersion.TryParse(version, out _))
            {
                return null;
            }

            return new UpdateFeedEntry(
                version!,
                ReadString(document.RootElement, "msixUrl"),
                ReadString(document.RootElement, "appinstallerUrl"),
                ReadString(document.RootElement, "sha256"));
        }
        catch (JsonException)
        {
            return null;
        }
    }

    private static string? ReadString(JsonElement element, string propertyName) =>
        element.TryGetProperty(propertyName, out var property) && property.ValueKind == JsonValueKind.String
            ? property.GetString()
            : null;
}

public enum UpdateCheckStatus
{
    /// <summary>No feed URL configured — the check is off (the default).</summary>
    Disabled,

    /// <summary>Feed fetched and parsed; current version is up to date (or ahead).</summary>
    UpToDate,

    /// <summary>Feed advertises a strictly newer version.</summary>
    UpdateAvailable,

    /// <summary>Network/parse failure. NEVER surfaced to the operator — log and move on
    /// (§7: the update check is non-blocking; failures are silent).</summary>
    Failed
}

public sealed record UpdateCheckResult(UpdateCheckStatus Status, UpdateFeedEntry? Update, string? Detail)
{
    public static UpdateCheckResult Disabled() => new(UpdateCheckStatus.Disabled, null, null);
    public static UpdateCheckResult UpToDate(UpdateFeedEntry entry) => new(UpdateCheckStatus.UpToDate, entry, null);
    public static UpdateCheckResult Available(UpdateFeedEntry entry) => new(UpdateCheckStatus.UpdateAvailable, entry, null);
    public static UpdateCheckResult Failed(string detail) => new(UpdateCheckStatus.Failed, null, detail);
}

/// <summary>
/// Persists the version the operator dismissed ("remind me later") so the
/// banner is not re-shown for the same version. Deliberately a tiny standalone
/// flag file (NOT ProductionOutputPreferences — that store carries production
/// state and is versioned/migrated independently).
/// </summary>
public sealed class DismissedUpdateVersionStore
{
    private static readonly JsonSerializerOptions JsonOptions = new(JsonSerializerDefaults.Web)
    {
        WriteIndented = true
    };

    private readonly string _filePath;

    public DismissedUpdateVersionStore(string filePath)
    {
        _filePath = filePath;
    }

    public static string DefaultStorePath() => Path.Combine(
        Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData),
        "CoreVideoPro",
        "update-dismissed.json");

    private sealed record Payload(string? DismissedVersion);

    public string? Load()
    {
        if (!File.Exists(_filePath))
        {
            return null;
        }

        try
        {
            var payload = JsonSerializer.Deserialize<Payload>(File.ReadAllText(_filePath), JsonOptions);
            return AppUpdateVersion.TryParse(payload?.DismissedVersion, out _) ? payload!.DismissedVersion : null;
        }
        catch
        {
            return null;
        }
    }

    public void Save(string version)
    {
        Directory.CreateDirectory(Path.GetDirectoryName(_filePath)!);
        File.WriteAllText(_filePath, JsonSerializer.Serialize(new Payload(version), JsonOptions));
    }

    public bool IsDismissed(string? candidateVersion) =>
        AppUpdateVersion.AreEqual(Load(), candidateVersion);
}

/// <summary>
/// Fetches `latest.json` from the configured feed URL and decides whether a
/// newer version exists. Pure logic + injectable HTTP handler; UI-free. The
/// caller (shell) is responsible for the launch delay, single-shot invocation,
/// and the non-blocking banner. Never throws — every failure folds into
/// <see cref="UpdateCheckStatus.Failed"/>.
/// </summary>
public sealed class UpdateCheckService : IDisposable
{
    public const string FeedUrlEnvironmentVariable = "COREVIDEO_UPDATE_FEED_URL";

    private static readonly TimeSpan RequestTimeout = TimeSpan.FromSeconds(10);

    private readonly HttpClient _http;

    public UpdateCheckService(HttpMessageHandler? handler = null)
    {
        _http = handler is null ? new HttpClient() : new HttpClient(handler);
        _http.Timeout = RequestTimeout;
    }

    public async Task<UpdateCheckResult> CheckAsync(
        string? currentVersion,
        string? feedUrl,
        CancellationToken cancellationToken = default)
    {
        if (string.IsNullOrWhiteSpace(feedUrl))
        {
            return UpdateCheckResult.Disabled();
        }

        if (!AppUpdateVersion.TryParse(currentVersion, out _))
        {
            return UpdateCheckResult.Failed($"current version is not parseable: \"{currentVersion}\"");
        }

        string json;
        try
        {
            json = await FetchFeedAsync(feedUrl.Trim(), cancellationToken).ConfigureAwait(false);
        }
        catch (Exception ex)
        {
            return UpdateCheckResult.Failed($"feed fetch failed: {ex.Message}");
        }

        var entry = UpdateFeedParser.TryParse(json);
        if (entry is null)
        {
            return UpdateCheckResult.Failed("latest.json is malformed (no parseable version)");
        }

        return AppUpdateVersion.IsNewer(entry.Version, currentVersion)
            ? UpdateCheckResult.Available(entry)
            : UpdateCheckResult.UpToDate(entry);
    }

    private async Task<string> FetchFeedAsync(string feedUrl, CancellationToken cancellationToken)
    {
        // file:// URLs and plain local paths are supported for rig testing
        // (point COREVIDEO_UPDATE_FEED_URL at a local latest.json).
        if (Uri.TryCreate(feedUrl, UriKind.Absolute, out var uri))
        {
            if (uri.IsFile)
            {
                return await File.ReadAllTextAsync(uri.LocalPath, cancellationToken).ConfigureAwait(false);
            }

            if (uri.Scheme == Uri.UriSchemeHttp || uri.Scheme == Uri.UriSchemeHttps)
            {
                using var response = await _http.GetAsync(uri, cancellationToken).ConfigureAwait(false);
                response.EnsureSuccessStatusCode();
                return await response.Content.ReadAsStringAsync(cancellationToken).ConfigureAwait(false);
            }
        }

        if (File.Exists(feedUrl))
        {
            return await File.ReadAllTextAsync(feedUrl, cancellationToken).ConfigureAwait(false);
        }

        throw new InvalidOperationException($"unsupported feed URL \"{feedUrl}\" (expect https://, file://, or a local path)");
    }

    public void Dispose() => _http.Dispose();
}
