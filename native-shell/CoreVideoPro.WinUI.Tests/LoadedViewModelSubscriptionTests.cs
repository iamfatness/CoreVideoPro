using System.Collections.ObjectModel;
using System.ComponentModel;
using CoreVideoPro.WinUI.Services;
using Xunit;

namespace CoreVideoPro.WinUI.Tests;

public sealed class LoadedViewModelSubscriptionTests
{
    private sealed class Scene : INotifyPropertyChanged
    {
        public event PropertyChangedEventHandler? PropertyChanged;
        public ObservableCollection<string> Layers { get; } = [];
        public void ChangeSource() => PropertyChanged?.Invoke(this, new("PreviewCanvasLayers"));
    }

    [Fact]
    public void ReloadSameSceneRestoresSourceAndCollectionUpdatesExactlyOnce()
    {
        var scene = new Scene();
        var updates = 0;
        PropertyChangedEventHandler properties = (_, _) => updates++;
        System.Collections.Specialized.NotifyCollectionChangedEventHandler layers = (_, _) => updates++;
        var subscriptions = new LoadedViewModelSubscription<Scene>(
            vm => { vm.PropertyChanged += properties; vm.Layers.CollectionChanged += layers; },
            vm => { vm.PropertyChanged -= properties; vm.Layers.CollectionChanged -= layers; });
        subscriptions.SetViewModel(scene);
        scene.ChangeSource();
        Assert.Equal(0, updates);
        for (var cycle = 0; cycle < 3; cycle++)
        {
            subscriptions.Load(scene);
            subscriptions.Load(scene);
            subscriptions.SetViewModel(scene);
            scene.ChangeSource();
            scene.Layers.Add("camera");
            Assert.Equal((cycle + 1) * 2, updates);
            subscriptions.Unload();
            subscriptions.Unload();
            scene.ChangeSource();
            scene.Layers.Add("hidden edit");
            Assert.Equal((cycle + 1) * 2, updates);
        }
    }

    [Fact]
    public void ReplacementWhileUnloadedSubscribesOnlyLatestSceneOnLoad()
    {
        var first = new Scene();
        var second = new Scene();
        var updates = new List<object?>();
        PropertyChangedEventHandler handler = (sender, _) => updates.Add(sender);
        var subscriptions = new LoadedViewModelSubscription<Scene>(
            vm => vm.PropertyChanged += handler, vm => vm.PropertyChanged -= handler);
        subscriptions.Load(first);
        subscriptions.Unload();
        subscriptions.SetViewModel(second);
        first.ChangeSource();
        second.ChangeSource();
        Assert.Empty(updates);
        subscriptions.Load(second);
        first.ChangeSource();
        second.ChangeSource();
        Assert.Same(second, Assert.Single(updates));
        subscriptions.SetViewModel(first);
        second.ChangeSource();
        first.ChangeSource();
        Assert.Equal(2, updates.Count);
        Assert.Same(first, updates[1]);
        subscriptions.SetViewModel(null);
        first.ChangeSource();
        Assert.Equal(2, updates.Count);
    }
}
