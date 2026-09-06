namespace CoreVideoPro.WinUI.Services;

/// <summary>Owns presentation only while a loaded view needs a GPU surface.</summary>
public sealed class VideoSurfacePresentationLifecycle(Func<bool> attach, Action detach)
{
    private bool _loaded;
    private bool _attached;

    public void Load(bool needsGpu)
    {
        _loaded = true;
        Refresh(needsGpu);
    }

    public void Refresh(bool needsGpu)
    {
        if (!_loaded) return;
        if (!needsGpu && _attached)
        {
            _attached = false;
            detach();
        }
        else if (needsGpu && !_attached)
        {
            _attached = attach();
        }
    }

    public void Unload()
    {
        _loaded = false;
        if (!_attached) return;
        _attached = false;
        detach();
    }
}
