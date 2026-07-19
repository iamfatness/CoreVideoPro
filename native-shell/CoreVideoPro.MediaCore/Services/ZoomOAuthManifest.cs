using System.Text.Json;

namespace CoreVideoPro.MediaCore.Services;

public sealed class ZoomOAuthManifest
{
    public const string DefaultBrokerStartUrl = "https://corevideo.iamfatness.us/oauth/start";
    public const string DefaultBrokerCallbackUrl = "https://corevideo.iamfatness.us/oauth/callback";
    // Pinned by the deployed broker: corevideo.iamfatness.us (site-worker.js,
    // handleOauthStart) rejects any return_uri other than exactly this value with
    // 400 "Unsupported OAuth return URI." — do NOT change without updating the
    // broker allowlist first. Package.appxmanifest declares the matching
    // "corevideo" protocol (plus a legacy "corevideopro" alias).
    public const string DefaultAppReturnUri = "corevideo://oauth/callback";
    public const string DefaultPublicClientId = "y6sIWSwiTZe1JygMx4C9EQ";

    public string BrokerStartUrl { get; init; } = string.Empty;
    public string BrokerCallbackUrl { get; init; } = DefaultBrokerCallbackUrl;
    public string RedirectUri { get; init; } = DefaultAppReturnUri;
    public string PublicClientId { get; init; } = DefaultPublicClientId;
    public string Scopes { get; init; } = "user:read:token user:read:user";

    public bool BrokerConfigured => BrokerStartUrl.Trim().Length > 0;
    public bool PublicClientIdPresent => PublicClientId.Trim().Length > 0;

    public static ZoomOAuthManifest Load(string? repoRoot = null)
    {
        repoRoot ??= MediaCorePaths.RepoRoot;
        var manifest = LoadFromFile(repoRoot) ?? CreateDefaults();

        var brokerOverride = Environment.GetEnvironmentVariable(MediaCorePaths.ZoomOAuthBrokerStartUrlEnvVar);
        if (string.IsNullOrWhiteSpace(brokerOverride))
        {
            return manifest;
        }

        return new ZoomOAuthManifest
        {
            BrokerStartUrl = brokerOverride.Trim(),
            BrokerCallbackUrl = manifest.BrokerCallbackUrl,
            RedirectUri = manifest.RedirectUri,
            PublicClientId = manifest.PublicClientId,
            Scopes = manifest.Scopes
        };
    }

    private static ZoomOAuthManifest? LoadFromFile(string repoRoot)
    {
        var manifestPath = Path.Combine(repoRoot, "src", "config", "zoomOAuth.json");
        if (!File.Exists(manifestPath))
        {
            return null;
        }

        try
        {
            using var document = JsonDocument.Parse(File.ReadAllText(manifestPath));
            var root = document.RootElement;
            return new ZoomOAuthManifest
            {
                BrokerStartUrl = ReadString(root, "brokerStartUrl", DefaultBrokerStartUrl),
                BrokerCallbackUrl = ReadString(root, "brokerCallbackUrl", DefaultBrokerCallbackUrl),
                RedirectUri = ReadString(root, "redirectUri", DefaultAppReturnUri),
                PublicClientId = ReadString(root, "publicClientId", DefaultPublicClientId),
                Scopes = ReadString(root, "scopes", "user:read:token user:read:user")
            };
        }
        catch
        {
            return null;
        }
    }

    private static ZoomOAuthManifest CreateDefaults() =>
        new()
        {
            BrokerStartUrl = DefaultBrokerStartUrl,
            BrokerCallbackUrl = DefaultBrokerCallbackUrl,
            RedirectUri = DefaultAppReturnUri,
            PublicClientId = DefaultPublicClientId
        };

    private static string ReadString(JsonElement root, string property, string fallback) =>
        root.TryGetProperty(property, out var element)
            ? element.GetString()?.Trim() ?? fallback
            : fallback;
}