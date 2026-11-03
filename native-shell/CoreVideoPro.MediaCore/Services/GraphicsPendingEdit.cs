namespace CoreVideoPro.MediaCore.Services;

/// <summary>One typed-but-uncommitted edit (the Designer's Fixed text box) and the layer it belongs to. The surface
/// flushes it before the selection moves, before the inspector is rebuilt and before a save, so text typed without
/// Enter lands on its own layer. <see cref="Flush"/> runs the commit at most once, even when the commit's own
/// refresh flushes again.</summary>
public sealed class GraphicsPendingEdit
{
    private Func<bool>? _commit;
    public string Owner { get; private set; } = "";

    public void Track(string ownerId, Func<bool> commit) { Owner = ownerId ?? ""; _commit = commit; }
    public void Clear() { Owner = ""; _commit = null; }

    /// <summary>Runs and forgets the pending commit; false when there was none (or it applied nothing).</summary>
    public bool Flush()
    {
        var commit = _commit; Clear();
        return commit is not null && commit();
    }
}
