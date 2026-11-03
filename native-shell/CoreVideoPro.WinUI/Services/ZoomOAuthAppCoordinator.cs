using CoreVideoPro.MediaCore.Services;
using Microsoft.UI.Dispatching;
using Microsoft.Windows.AppLifecycle;

namespace CoreVideoPro.WinUI.Services;

public sealed class ZoomOAuthAppCoordinator
{
    // "corevideo" is THE OAuth app-return scheme: the deployed broker
    // (corevideo.iamfatness.us site-worker.js) hard-rejects any return_uri other
    // than corevideo://oauth/callback, and ZoomOAuthManifest.DefaultAppReturnUri
    // + Package.appxmanifest both declare it. "corevideopro" is kept as a legacy
    // alias only (older HKCU registrations / docs).
    public const string Protocol = "corevideo";
    public const string LegacyProtocol = "corevideopro";
    public static readonly string[] SupportedProtocols = [Protocol, LegacyProtocol];

    private readonly ZoomOAuthService _oauth;
    private readonly DispatcherQueue _dispatcher;
    private Action<string>? _statusChanged;

    public ZoomOAuthAppCoordinator(ZoomOAuthService oauth, DispatcherQueue dispatcher)
    {
        _oauth = oauth;
        _dispatcher = dispatcher;
    }

    public ZoomOAuthService OAuth => _oauth;

    public void SetStatusChangedHandler(Action<string>? handler) => _statusChanged = handler;

    public void Initialize()
    {
        AppInstance.GetCurrent().Activated += OnAppActivated;
        TryHandleActivationArguments(Environment.GetCommandLineArgs());
        TryDrainPendingCallback();
    }

    public void TryDrainPendingCallback()
    {
        var pending = OAuthCallbackRelay.TryConsume();
        if (!string.IsNullOrWhiteSpace(pending))
        {
            TryHandleOAuthCallback(pending);
        }
    }

    public void Dispose()
    {
        AppInstance.GetCurrent().Activated -= OnAppActivated;
    }

    public bool TryRegisterProtocolHandler(string exePath, out string? error)
    {
        error = null;
        try
        {
            foreach (var protocol in SupportedProtocols)
            {
                using var protocolKey = Microsoft.Win32.Registry.CurrentUser.CreateSubKey($@"Software\Classes\{protocol}");
                protocolKey.SetValue("", $"URL:{protocol} Protocol");
                protocolKey.SetValue("URL Protocol", "");
                using var commandKey = protocolKey.CreateSubKey(@"shell\open\command");
                commandKey.SetValue("", $"\"{exePath}\" \"%1\"");
            }

            return true;
        }
        catch (Exception ex)
        {
            error = ex.Message;
            return false;
        }
    }

    private void OnAppActivated(object? sender, AppActivationArguments args) =>
        HandleActivationArguments(args);

    private void TryHandleActivationArguments(string[] args)
    {
        var callback = args.FirstOrDefault(IsOAuthCallbackUrl);
        if (!string.IsNullOrWhiteSpace(callback))
        {
            TryHandleOAuthCallback(callback);
        }
    }

    public static bool IsOAuthCallbackUrl(string? value) =>
        !string.IsNullOrWhiteSpace(value) &&
        SupportedProtocols.Any(protocol =>
            value.StartsWith($"{protocol}://", StringComparison.OrdinalIgnoreCase));

    public void TryHandleOAuthCallback(string? callbackUrl)
    {
        if (!IsOAuthCallbackUrl(callbackUrl))
        {
            return;
        }

        LaunchLog.Write("oauth: callback received");
        _ = Task.Run(async () =>
        {
            try
            {
                await _oauth.HandleRedirectUrlAsync(callbackUrl).ConfigureAwait(false);
                LaunchLog.Write("oauth: sign-in completed");
                _dispatcher.TryEnqueue(() => _statusChanged?.Invoke("Signed in with Zoom."));
            }
            catch (Exception ex)
            {
                LaunchLog.Write($"oauth: callback failed: {ex.Message}");
                _dispatcher.TryEnqueue(() => _statusChanged?.Invoke(ex.Message));
            }
        });
    }

    public void HandleActivationArguments(AppActivationArguments args)
    {
        LaunchLog.Write($"activation: kind={args.Kind}");
        var callback = OAuthCallbackRelay.ExtractFromActivation(args);
        if (!string.IsNullOrWhiteSpace(callback))
        {
            TryHandleOAuthCallback(callback);
            return;
        }

        TryHandleActivationArguments(Environment.GetCommandLineArgs());
        TryDrainPendingCallback();
    }
}