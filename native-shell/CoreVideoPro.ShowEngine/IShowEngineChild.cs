namespace CoreVideoPro.ShowEngine;

/// <summary>
/// The process seam the supervisor is tested through. One instance == one spawned engine process
/// (one generation). The real implementation is <see cref="ProcessShowEngineChild"/>; tests use an
/// in-memory fake, which is the only reason the supervisor's exit/hang/stale-generation behavior is
/// coverable at all without Node on the box.
/// </summary>
public interface IShowEngineChild : IDisposable
{
    /// <summary>The generation this child was spawned as — also the <c>--generation</c> it was given,
    /// which the engine echoes back on every event it emits.</summary>
    int Generation { get; }

    Task WriteLineAsync(string line, CancellationToken ct);

    /// <summary>Completes with the next stdout line, or null at EOF.</summary>
    Task<string?> ReadLineAsync(CancellationToken ct);

    /// <summary>Completes with the exit code when the process ends. Never faults.</summary>
    Task<int> Exited { get; }

    void Kill();
}

public interface IShowEngineChildFactory
{
    IShowEngineChild Spawn(int generation, ShowEngineSpawnRequest request);
}

/// <summary>Everything needed to launch one engine process. Resolved by <c>ShowEnginePaths</c>
/// (spec §6.4) outside this project; the supervisor just carries it to the factory.</summary>
public sealed record ShowEngineSpawnRequest(
    string NodeExe,
    string EntryScript,
    string ConfigPath,
    string WorkingDirectory,
    IReadOnlyDictionary<string, string> Environment)
{
    /// <summary>Extra argv appended AFTER the fixed <c>--config &lt;path&gt; --generation &lt;n&gt;</c>
    /// pair. Empty for a normal show. Its one production-adjacent use is the host's
    /// <c>--conformance</c> mode (Plan 7a Task 13), which the WinUI adapter conformance test spawns
    /// through this same real supervisor + child rather than a second, differently-shaped launcher —
    /// a test that spawned the engine its own way would stop proving the spawn path works.</summary>
    public IReadOnlyList<string> ExtraArgs { get; init; } = Array.Empty<string>();
}
