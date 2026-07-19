using System.Reflection;
using CoreVideoPro.MediaCore.Services;
using Microsoft.UI.Dispatching;

namespace CoreVideoPro.WinUI.Services;

/// <summary>
/// One-shot, non-blocking startup update check (beta spec D4, belt-and-
/// suspenders next to the App Installer OnLaunch check — and the whole
/// mechanism if distribution goes EXE-installer).
///
/// Posture (spec §7 invariants): the check runs a few seconds AFTER launch,
/// entirely off the UI thread; the feed URL comes from
/// COREVIDEO_UPDATE_FEED_URL (empty default = disabled); every failure —
/// network, malformed latest.json, anything — is silently logged and NEVER
/// surfaced, nagged, or blocking. At most one check per launch. A dismissed
/// version is persisted in a standalone app-data flag file and never re-shown.
/// The banner itself is a static one-shot InfoBar in MainWindow (no
/// snapshot-driven collections — 0xc000027b rules).
/// </summary>
public static class UpdateNotificationService
{
    public sealed record UpdateOffer(string CurrentVersion, string NewVersion, string DownloadUrl);

    private static readonly TimeSpan StartupDelay = TimeSpan.FromSeconds(5);
    private static int _started;

    /// <summary>Kick off the single launch-time check; safe to call more than once.</summary>
    public static void Start(DispatcherQueue dispatcher, Action<UpdateOffer> showBanner)
    {
        if (Interlocked.Exchange(ref _started, 1) != 0)
        {
            return; // at most one check per launch
        }

        _ = Task.Run(() => RunOnceAsync(dispatcher, showBanner));
    }

    private static async Task RunOnceAsync(DispatcherQueue dispatcher, Action<UpdateOffer> showBanner)
    {
        try
        {
            await Task.Delay(StartupDelay).ConfigureAwait(false);

            var feedUrl = Environment.GetEnvironmentVariable(UpdateCheckService.FeedUrlEnvironmentVariable);
            if (string.IsNullOrWhiteSpace(feedUrl))
            {
                return; // update check disabled (the default until hosting exists)
            }

            var current = ResolveCurrentVersion();
            using var service = new UpdateCheckService();
            var result = await service.CheckAsync(current, feedUrl).ConfigureAwait(false);

            if (result.Status == UpdateCheckStatus.Failed)
            {
                LaunchLog.Write($"update: check failed silently ({result.Detail})");
                return;
            }

            if (result.Status != UpdateCheckStatus.UpdateAvailable)
            {
                LaunchLog.Write($"update: up to date (v{current})");
                return;
            }

            var update = result.Update!;
            var downloadUrl = update.DownloadUrl;
            if (string.IsNullOrWhiteSpace(downloadUrl))
            {
                LaunchLog.Write($"update: v{update.Version} advertised without a download URL — staying quiet");
                return;
            }

            var store = new DismissedUpdateVersionStore(DismissedUpdateVersionStore.DefaultStorePath());
            if (store.IsDismissed(update.Version))
            {
                LaunchLog.Write($"update: v{update.Version} available but previously dismissed — staying quiet");
                return;
            }

            LaunchLog.Write($"update: v{update.Version} available (current v{current}) — surfacing banner");
            dispatcher.TryEnqueue(() => showBanner(new UpdateOffer(current, update.Version, downloadUrl)));
        }
        catch (Exception ex)
        {
            // The update check must never take the app down or nag; log and move on.
            try { LaunchLog.Write($"update: check crashed silently ({ex.GetType().Name}: {ex.Message})"); } catch { }
        }
    }

    /// <summary>"Remind me later": suppress this version for future launches.</summary>
    public static void RecordDismissed(string version) =>
        _ = Task.Run(() =>
        {
            try
            {
                new DismissedUpdateVersionStore(DismissedUpdateVersionStore.DefaultStorePath()).Save(version);
                LaunchLog.Write($"update: v{version} dismissed — will not re-show for this version");
            }
            catch (Exception ex)
            {
                try { LaunchLog.Write($"update: could not persist dismissal ({ex.Message})"); } catch { }
            }
        });

    /// <summary>Open the appinstaller/download URL in the default browser.</summary>
    public static void OpenDownload(string url) =>
        _ = Task.Run(async () =>
        {
            try
            {
                await ExternalUriLauncher.OpenAsync(url).ConfigureAwait(false);
            }
            catch (Exception ex)
            {
                try { LaunchLog.Write($"update: could not open download URL ({ex.Message})"); } catch { }
            }
        });

    /// <summary>
    /// Current app version: package identity when running packaged (MSIX);
    /// assembly (informational) version when unpackaged.
    /// </summary>
    public static string ResolveCurrentVersion()
    {
        try
        {
            var packaged = Windows.ApplicationModel.Package.Current.Id.Version;
            return $"{packaged.Major}.{packaged.Minor}.{packaged.Build}";
        }
        catch
        {
            // No package identity (WindowsPackageType=None dev/loose runs).
        }

        var assembly = Assembly.GetEntryAssembly() ?? typeof(UpdateNotificationService).Assembly;
        var informational = assembly
            .GetCustomAttribute<AssemblyInformationalVersionAttribute>()?
            .InformationalVersion;
        if (!string.IsNullOrWhiteSpace(informational))
        {
            var metadataStart = informational.IndexOf('+');
            var candidate = metadataStart >= 0 ? informational[..metadataStart] : informational;
            if (AppUpdateVersion.TryParse(candidate, out _))
            {
                return candidate;
            }
        }

        var assemblyVersion = assembly.GetName().Version;
        return assemblyVersion is null
            ? "0.0.0"
            : $"{assemblyVersion.Major}.{assemblyVersion.Minor}.{Math.Max(0, assemblyVersion.Build)}";
    }
}
