namespace CoreVideoPro.WinUI.Services;

// UI-thread policy shared by scene-selection notifications and gallery edits.
// Model-to-control notifications may echo through TwoWay setters; those echoes
// must not mutate settings or schedule persistence/native synchronization.
internal sealed class GalleryEditGuard
{
    private int _depth;
    internal bool TryEdit(Action edit)
    {
        if (_depth != 0) return false;
        Notify(edit);
        return true;
    }

    internal void Notify(Action notify)
    {
        _depth++;
        try { notify(); }
        finally { _depth--; }
    }
}
