namespace CoreVideoPro.WinUI.Services;

/// <summary>All aliases of one keyed-mutex handle have one owning render thread.</summary>
internal sealed class SharedTextureRenderAssignments(int workers)
{
    private sealed class Assignment(int worker) { internal readonly int Worker = worker; internal int References; }
    private readonly object _gate = new();
    private readonly int[] _loads = workers > 0 ? new int[workers] : throw new ArgumentOutOfRangeException(nameof(workers));
    private readonly Dictionary<ulong, Assignment> _handles = new();
    internal int Acquire(ulong handle)
    {
        lock (_gate)
        {
            if (!_handles.TryGetValue(handle, out var assignment))
            {
                var least = 0;
                for (var i = 1; i < _loads.Length; i++) if (_loads[i] < _loads[least]) least = i;
                assignment = new(least); _handles.Add(handle, assignment);
            }
            assignment.References++; _loads[assignment.Worker]++;
            return assignment.Worker;
        }
    }
    internal void Release(ulong handle, int worker)
    {
        lock (_gate)
        {
            var assignment = _handles[handle];
            if (assignment.Worker != worker) throw new InvalidOperationException("Wrong render owner");
            _loads[worker]--;
            if (--assignment.References == 0) _handles.Remove(handle);
        }
    }
    internal int[] Loads { get { lock (_gate) return (int[])_loads.Clone(); } }
}
