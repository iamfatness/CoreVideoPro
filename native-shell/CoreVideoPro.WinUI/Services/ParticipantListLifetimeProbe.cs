using System.Diagnostics;
using CoreVideoPro.MediaCore.Services;
using CoreVideoPro.WinUI.Models;
using CoreVideoPro.WinUI.ViewModels;
using CoreVideoPro.WinUI.Views;
using Microsoft.UI.Dispatching;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;

namespace CoreVideoPro.WinUI.Services;

// Uses the compiled workspace, real Loaded/Unloaded events and real dispatcher.
// The scheduler unit tests separately pin both callback interleavings exactly.
internal static class ParticipantListLifetimeProbe
{
    internal static async Task CheckAsync(Window window, StudioWorkspace page, StudioViewModel original,
        List<string> checks, List<string> failures)
    {
        await using var replacement = new StudioViewModel(StudioStartupBootstrap.Create(
            new InMemoryProductionOutputPreferencesStore(), startInteractiveServices: false));
        var list = (ListView)page.FindName("ParticipantListView");
        var dispatcher = DispatcherQueue.GetForCurrentThread();
        for (var cycle = 0; cycle < 6; cycle++)
        {
            page.ViewModel = original;
            await DrainAsync(dispatcher);
            var sentinel = new List<ParticipantListItem>();
            var unloaded = false;
            void OnUnloaded(object sender, RoutedEventArgs args)
            {
                unloaded = true;
                list.ItemsSource = sentinel;
            }
            page.Unloaded += OnUnloaded;
            try
            {
                // A real ViewModel replacement queues the production refresh.
                page.ViewModel = replacement;
                if (cycle % 2 == 0) window.Content = null;
                else UiDispatch.Enqueue(dispatcher, DispatcherQueuePriority.Low,
                    () => window.Content = null, "ParticipantListLifetimeProbe.detach-after-clear");
                await WaitUntilAsync(() => unloaded, "Studio did not unload");
                await DrainAsync(dispatcher);
                if (!ReferenceEquals(list.ItemsSource, sentinel))
                    failures.Add($"Participant list cycle {cycle}: queued work touched the unloaded list");

                // Cover both a replacement while detached and the same model
                // surviving unload (which must reconnect its notifications).
                var reloaded = cycle % 2 == 0 ? original : replacement;
                page.ViewModel = reloaded;
                window.Content = page;
                await WaitUntilAsync(() => page.IsLoaded, "Studio did not reload");
                await DrainAsync(dispatcher);
                if (!ReferenceEquals(list.ItemsSource, reloaded.ParticipantListItems))
                    failures.Add($"Participant list cycle {cycle}: reload did not bind the latest roster");
                reloaded.ActiveTab = StudioTab.Overlays;
                if (((FrameworkElement)page.FindName("ProductionFooter")).Visibility != Visibility.Collapsed)
                    failures.Add($"Participant list cycle {cycle}: reload did not reconnect ViewModel notifications");
                reloaded.ActiveTab = StudioTab.Studio;
            }
            finally { page.Unloaded -= OnUnloaded; }
        }
        // Normal app shutdown detaches before Unloaded, with queued work possible.
        page.ViewModel = replacement;
        page.PrepareForShutdown();
        var shutdownSentinel = new List<ParticipantListItem>();
        list.ItemsSource = shutdownSentinel;
        await DrainAsync(dispatcher);
        if (!ReferenceEquals(list.ItemsSource, shutdownSentinel))
            failures.Add("Participant list: queued work touched the list after shutdown preparation");
        // Leave no live XAML subscriber when the probe disposes its ViewModels.
        window.Content = null;
        await WaitUntilAsync(() => !page.IsLoaded, "Studio final unload failed");
        await DrainAsync(dispatcher);
        checks.Add("Compiled participant list: six detach/reload cycles preserve unloaded ItemsSource, bind latest roster and reconnect notifications; shutdown preparation invalidates pending work before Unloaded");
    }

    private static async Task WaitUntilAsync(Func<bool> condition, string failure)
    {
        var watch = Stopwatch.StartNew();
        while (!condition() && watch.Elapsed < TimeSpan.FromSeconds(5)) await Task.Delay(20);
        if (!condition()) throw new InvalidOperationException(failure);
    }

    private static async Task DrainAsync(DispatcherQueue dispatcher)
    {
        // Two turns cover clear followed by the separately queued rebind.
        for (var i = 0; i < 3; i++)
        {
            var completed = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
            UiDispatch.Enqueue(dispatcher, DispatcherQueuePriority.Low, () => completed.SetResult(),
                "ParticipantListLifetimeProbe.drain");
            await completed.Task.WaitAsync(TimeSpan.FromSeconds(5));
        }
    }
}
