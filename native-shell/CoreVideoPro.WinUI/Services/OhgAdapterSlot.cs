namespace CoreVideoPro.WinUI.Services;

/// <summary>
/// Publishes the adapter and its minimum engine generation as one immutable binding.
/// Commands capture the binding before UI dispatch and may apply only while it is
/// still current. A settings save retires queued commands instead of replaying old
/// payloads through the new configuration. Readers never wait on the UI thread.
/// </summary>
public sealed class OhgAdapterSlot
{
    public sealed record Binding(OhgHostAdapter? Adapter, long MinimumGeneration);
    private Binding _binding = new(null, 0);

    public OhgHostAdapter? Current => Capture().Adapter;
    public Binding Capture() => Volatile.Read(ref _binding);

    public void Replace(OhgHostAdapter? adapter, long minimumGeneration = 0)
        => Volatile.Write(ref _binding, new Binding(adapter, minimumGeneration));

    public OhgHostAdapter? Resolve(Binding captured, int commandGeneration, int currentGeneration)
        => ReferenceEquals(captured, Capture()) && commandGeneration == currentGeneration &&
           commandGeneration >= captured.MinimumGeneration ? captured.Adapter : null;
}
