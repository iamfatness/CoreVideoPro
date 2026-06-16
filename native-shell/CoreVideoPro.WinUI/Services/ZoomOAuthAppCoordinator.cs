using CoreVideoPro.MediaCore.Services;
using Microsoft.UI.Dispatching;
using Microsoft.Windows.AppLifecycle;

namespace CoreVideoPro.WinUI.Services;

public sealed class ZoomOAuthAppCoordinator
{
    public const string Protocol = "corevideopro";

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
            using var protocolKey = Microsoft.Win32.Registry.CurrentUser.CreateSubKey($@"Software\Classes\{Protocol}");
            protocolKey.SetValue("", $"URL:{Protocol} Protocol");
            protocolKey.SetValue("URL Protocol", "");
            using var commandKey = protocolKey.CreateSubKey(@"shell\open\command");
            commandKey.SetValue("", $"\"{exePath}\" \"%1\"");
            return true;
        }
        catch (Exception ex)
        {
            error = ex.Message;
            return false;
        }
    }

    private void OnAppActivated(object? sender, AppActivationArguments args)
    {
        if (args.Kind != ExtendedActivationKind.Protocol)
        {
            return;
        }

        if (args.Data is not Windows.ApplicationModel.Activation.IProtocolActivatedEventArgs protocolArgs)
        {
            return;
        }

        TryHandleOAuthCallback(protocolArgs.Uri?.ToString());
    }

    private void TryHandleActivationArguments(string[] args)
    {
        var callback = args.FirstOrDefault(arg =>
            arg.StartsWith($"{Protocol}://", StringComparison.OrdinalIgnoreCase));
        if (!string.IsNullOrWhiteSpace(callback))
        {
            TryHandleOAuthCallback(callback);
        }
    }

    private void TryHandleOAuthCallback(string? callbackUrl)
    {
        if (string.IsNullOrWhiteSpace(callbackUrl) ||
            !callbackUrl.StartsWith($"{Protocol}://", StringComparison.OrdinalIgnoreCase))
        {
            return;
        }

        _ = Task.Run(async () =>
        {
            try
            {
                await _oauth.HandleRedirectUrlAsync(callbackUrl).ConfigureAwait(false);
                _dispatcher.TryEnqueue(() => _statusChanged?.Invoke("Signed in with Zoom."));
            }
            catch (Exception ex)
            {
                _dispatcher.TryEnqueue(() => _statusChanged?.Invoke(ex.Message));
            }
        });
    }
}