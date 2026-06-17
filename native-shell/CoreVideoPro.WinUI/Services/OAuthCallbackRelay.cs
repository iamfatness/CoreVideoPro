using Microsoft.Windows.AppLifecycle;
using Windows.ApplicationModel.Activation;

namespace CoreVideoPro.WinUI.Services;

internal static class OAuthCallbackRelay
{
    private static readonly string PendingPath = Path.Combine(
        Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData),
        "CoreVideoPro",
        "oauth-callback.pending");

    public static void Persist(string callbackUrl)
    {
        if (!ZoomOAuthAppCoordinator.IsOAuthCallbackUrl(callbackUrl))
        {
            return;
        }

        Directory.CreateDirectory(Path.GetDirectoryName(PendingPath)!);
        File.WriteAllText(PendingPath, callbackUrl);
        LaunchLog.Write("oauth: callback persisted for primary instance");
    }

    public static string? TryConsume()
    {
        if (!File.Exists(PendingPath))
        {
            return null;
        }

        try
        {
            var callback = File.ReadAllText(PendingPath).Trim();
            File.Delete(PendingPath);
            return ZoomOAuthAppCoordinator.IsOAuthCallbackUrl(callback) ? callback : null;
        }
        catch (Exception ex)
        {
            LaunchLog.Write($"oauth: failed to consume pending callback: {ex.Message}");
            return null;
        }
    }

    public static string? ExtractFromActivation(AppActivationArguments args)
    {
        if (args.Kind == ExtendedActivationKind.Protocol &&
            args.Data is IProtocolActivatedEventArgs protocolArgs)
        {
            return protocolArgs.Uri?.ToString();
        }

        return AppActivationCoordinator.FindOAuthCallbackUrl(Environment.GetCommandLineArgs());
    }
}