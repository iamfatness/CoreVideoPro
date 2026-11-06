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

                // An unbound replacement must be picked up on the next load.
                page.ViewModel = original;
                window.Content = page;
                await WaitUntilAsync(() => page.IsLoaded, "Studio did not reload");
                await DrainAsync(dispatcher);
                if (!ReferenceEquals(list.ItemsSource, original.ParticipantListItems))
                    failures.Add($"Participant list cycle {cycle}: reload did not bind the latest roster");
                original.ActiveTab = StudioTab.Overlays;
                if (((FrameworkElement)page.FindName("ProductionFooter")).Visibility != Visibility.Collapsed)
                    failures.Add($"Participant list cycle {cycle}: reload did not reconnect ViewModel notifications");
                original.ActiveTab = StudioTab.Studio;
            }
            finally { page.Unloaded -= OnUnloaded; }
        }
        // Leave no live XAML subscriber when the probe disposes its ViewModels.
        window.Content = null;
        await WaitUntilAsync(() => !page.IsLoaded, "Studio final unload failed");
        await DrainAsync(dispatcher);
        checks.Add("Compiled participant list: six detach/reload cycles preserve unloaded ItemsSource, bind latest roster and reconnect notifications");
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
