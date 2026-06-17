using Microsoft.Windows.AppLifecycle;

namespace CoreVideoPro.WinUI.Services;

public sealed class AppActivationCoordinator
{
    public const string InstanceKey = "CoreVideoPro.WinUI.Primary";

    private readonly AppInstance _current = AppInstance.GetCurrent();
    private readonly AppInstance _primary;
    private Action<AppActivationArguments>? _onActivated;

    public AppActivationCoordinator()
    {
        _primary = AppInstance.FindOrRegisterForKey(InstanceKey);
    }

    public bool IsPrimaryInstance => _primary.IsCurrent;

    public AppActivationArguments CurrentActivationArguments => _current.GetActivatedEventArgs();

    public void SetActivationHandler(Action<AppActivationArguments>? handler) => _onActivated = handler;

    public void Subscribe()
    {
        _primary.Activated += OnPrimaryActivated;
    }

    public void Unsubscribe()
    {
        _primary.Activated -= OnPrimaryActivated;
    }

    public async Task<bool> TryRedirectToPrimaryAsync()
    {
        if (IsPrimaryInstance)
        {
            return false;
        }

        var activationArgs = CurrentActivationArguments;
        var callback = OAuthCallbackRelay.ExtractFromActivation(activationArgs);
        if (!string.IsNullOrWhiteSpace(callback))
        {
            OAuthCallbackRelay.Persist(callback);
        }

        LaunchLog.Write("activation: redirecting secondary instance to primary");
        await _primary.RedirectActivationToAsync(activationArgs).AsTask()
            .ConfigureAwait(false);
        return true;
    }

    public static string? FindOAuthCallbackUrl(IReadOnlyList<string> args) =>
        args.FirstOrDefault(ZoomOAuthAppCoordinator.IsOAuthCallbackUrl);

    private void OnPrimaryActivated(object? sender, AppActivationArguments args) =>
        _onActivated?.Invoke(args);
}