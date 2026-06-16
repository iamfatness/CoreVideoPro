using System.Text.Json;

namespace CoreVideoPro.MediaCore.Services;

public sealed class ZoomOAuthManifest
{
    public string BrokerStartUrl { get; init; } = string.Empty;
    public string RedirectUri { get; init; } = "corevideopro://oauth/callback";
    public string Scopes { get; init; } = "user:read:token user:read:user";

    public bool BrokerConfigured => BrokerStartUrl.Trim().Length > 0;

    public static ZoomOAuthManifest Load(string? repoRoot = null)
    {
        repoRoot ??= MediaCorePaths.RepoRoot;
        var envOverride = Environment.GetEnvironmentVariable(MediaCorePaths.ZoomOAuthBrokerStartUrlEnvVar);
        if (!string.IsNullOrWhiteSpace(envOverride))
        {
            return new ZoomOAuthManifest
            {
                BrokerStartUrl = envOverride.Trim(),
                RedirectUri = ResolveRedirectUri(repoRoot),
                Scopes = "user:read:token user:read:user"
            };
        }

        var manifestPath = Path.Combine(repoRoot, "src", "config", "zoomOAuth.json");
        if (!File.Exists(manifestPath))
        {
            return new ZoomOAuthManifest();
        }

        try
        {
            using var document = JsonDocument.Parse(File.ReadAllText(manifestPath));
            var root = document.RootElement;
            return new ZoomOAuthManifest
            {
                BrokerStartUrl = root.TryGetProperty("brokerStartUrl", out var broker) ? broker.GetString()?.Trim() ?? "" : "",
                RedirectUri = root.TryGetProperty("redirectUri", out var redirect) ? redirect.GetString()?.Trim() ?? "corevideopro://oauth/callback" : "corevideopro://oauth/callback",
                Scopes = root.TryGetProperty("scopes", out var scopes) ? scopes.GetString()?.Trim() ?? "user:read:token user:read:user" : "user:read:token user:read:user"
            };
        }
        catch
        {
            return new ZoomOAuthManifest();
        }
    }

    private static string ResolveRedirectUri(string repoRoot)
    {
        var manifestPath = Path.Combine(repoRoot, "src", "config", "zoomOAuth.json");
        if (!File.Exists(manifestPath))
        {
            return "corevideopro://oauth/callback";
        }

        try
        {
            using var document = JsonDocument.Parse(File.ReadAllText(manifestPath));
            if (document.RootElement.TryGetProperty("redirectUri", out var redirect))
            {
                return redirect.GetString()?.Trim() ?? "corevideopro://oauth/callback";
            }
        }
        catch
        {
            // ignore
        }

        return "corevideopro://oauth/callback";
    }
}