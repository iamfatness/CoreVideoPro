namespace CoreVideoPro.WinUI.Services;

/// <summary>Keeps event subscriptions attached only while a view is loaded.</summary>
internal sealed class LoadedViewModelSubscription<T>(Action<T> subscribe, Action<T> unsubscribe) where T : class
{
    private bool _loaded;
    private T? _viewModel;
    internal T? Current { get; private set; }

    internal void SetViewModel(T? viewModel)
    {
        _viewModel = viewModel;
        Rebind();
    }

    internal void Load(T? viewModel)
    {
        _loaded = true;
        SetViewModel(viewModel);
    }

    internal void Unload()
    {
        _loaded = false;
        Rebind();
    }

    private void Rebind()
    {
        var next = _loaded ? _viewModel : null;
        if (ReferenceEquals(Current, next)) return;
        if (Current is { } previous) unsubscribe(previous);
        Current = next;
        if (Current is { } current) subscribe(current);
    }
}
