namespace CoreVideoPro.MediaCore.Services;

/// <summary>Text typed into an Operate row's variable box (re-review m1). The box commits on Enter or LostFocus, and a
/// commit is an asynchronous playlist save. Show, Cue and Take call <see cref="BeforeAirAsync"/> first: it starts the
/// commit of text still being typed and waits for every save already running, so the action reads the typed value.
/// Commits run one after another, in the order they were made.</summary>
public sealed class GraphicsRowTextCommits
{
    private Func<Task>? _pending;
    private Task _inFlight = Task.CompletedTask;
    private readonly object _gate = new();

    /// <summary>The box (<paramref name="owner"/>, e.g. "row|location") whose typed text is not committed yet.</summary>
    public string Owner { get; private set; } = "";

    /// <summary>Remember the commit for text being typed; replaces any earlier pending text (the latest box wins).</summary>
    public void Track(string owner, Func<Task> commit) { lock (_gate) { Owner = owner ?? ""; _pending = commit; } }

    /// <summary>Forget the pending text without committing it (the box committed it itself, or nothing changed).</summary>
    public void Clear() { lock (_gate) { Owner = ""; _pending = null; } }

    /// <summary>Commit now (Enter or LostFocus): runs after the saves already running and forgets the pending text.</summary>
    public Task Commit(Func<Task> commit)
    {
        lock (_gate) {
            Owner = ""; _pending = null;
            var previous = _inFlight;
            return _inFlight = Chain(previous, commit);
        }
    }

    /// <summary>Starts the pending commit, if any (before the inspector is rebuilt); does not wait for it.</summary>
    public Task Flush()
    {
        Func<Task>? pending;
        lock (_gate) { pending = _pending; }
        return pending is null ? CurrentTask() : Commit(pending);
    }

    /// <summary>Before Show, Cue or Take: commit the pending text and wait until every row save has finished.</summary>
    public async Task BeforeAirAsync()
    {
        await Flush().ConfigureAwait(true);
        await CurrentTask().ConfigureAwait(true);
    }

    private Task CurrentTask() { lock (_gate) return _inFlight; }

    private static async Task Chain(Task previous, Func<Task> commit)
    {
        try { await previous.ConfigureAwait(true); } catch (Exception ex) when (ex is not OutOfMemoryException) { }
        await commit().ConfigureAwait(true);
    }
}
