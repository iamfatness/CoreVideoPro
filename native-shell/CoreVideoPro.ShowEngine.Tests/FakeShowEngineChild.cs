using System.Collections.Concurrent;
using System.Text.Json;
using System.Threading.Channels;
using CoreVideoPro.ShowEngine;

namespace CoreVideoPro.ShowEngine.Tests;

/// <summary>
/// In-memory <see cref="IShowEngineChild"/>. Stdout is a <see cref="Channel{T}"/> the test pushes
/// lines onto; stdin is recorded into <see cref="Written"/> and, when <see cref="Responder"/> is set,
/// each written line's scripted replies are pushed straight back onto stdout.
/// </summary>
public sealed class FakeShowEngineChild : IShowEngineChild
{
    private readonly Channel<string> _stdout = Channel.CreateUnbounded<string>();
    private readonly TaskCompletionSource<int> _exited =
        new(TaskCreationOptions.RunContinuationsAsynchronously);
    private readonly ConcurrentQueue<string> _written = new();

    public FakeShowEngineChild(int generation) => Generation = generation;

    public int Generation { get; }

    /// <summary>Every line the supervisor wrote to stdin, in order.</summary>
    public IReadOnlyList<string> Written => _written.ToArray();

    /// <summary>Scripted stdin → stdout replies. Return an empty sequence to answer nothing.</summary>
    public Func<string, IEnumerable<string>>? Responder { get; set; }

    /// <summary>When set, the child ends (stdout EOF + <see cref="Exited"/>) right after the next
    /// stdin write has pushed its scripted replies — how a test scripts "answers shutdown, then exits".</summary>
    public int? ExitAfterWrite { get; set; }

    public bool Killed { get; private set; }
    public bool Disposed { get; private set; }

    /// <summary>Preload / push a stdout line.</summary>
    public void Push(string line) => _stdout.Writer.TryWrite(line);

    /// <summary>Complete <see cref="Exited"/> only — stdout stays open (a still-readable stale child).</summary>
    public void SignalExit(int exitCode) => _exited.TrySetResult(exitCode);

    /// <summary>End stdout (EOF) and complete <see cref="Exited"/> — a real process death.</summary>
    public void Complete(int exitCode)
    {
        _exited.TrySetResult(exitCode);
        _stdout.Writer.TryComplete();
    }

    public Task WriteLineAsync(string line, CancellationToken ct)
    {
        ct.ThrowIfCancellationRequested();
        _written.Enqueue(line);
        var responder = Responder;
        if (responder is not null)
        {
            foreach (var reply in responder(line))
            {
                _stdout.Writer.TryWrite(reply);
            }
        }

        if (ExitAfterWrite is { } code)
        {
            ExitAfterWrite = null;
            Complete(code);
        }

        return Task.CompletedTask;
    }

    public async Task<string?> ReadLineAsync(CancellationToken ct)
    {
        try
        {
            return await _stdout.Reader.ReadAsync(ct).ConfigureAwait(false);
        }
        catch (ChannelClosedException)
        {
            return null;
        }
    }

    public Task<int> Exited => _exited.Task;

    public void Kill()
    {
        Killed = true;
        Complete(-1);
    }

    public void Dispose()
    {
        Disposed = true;
        _stdout.Writer.TryComplete();
        _exited.TrySetResult(-1);
    }

    // ---------------------------------------------------------------- helpers

    /// <summary>The `id` of the first written line whose `type` matches, or null.</summary>
    public string? IdOfWritten(string type)
    {
        foreach (var line in Written)
        {
            using var doc = JsonDocument.Parse(line);
            if (doc.RootElement.TryGetProperty("type", out var t) && t.GetString() == type &&
                doc.RootElement.TryGetProperty("id", out var id))
            {
                return id.GetString();
            }
        }

        return null;
    }

    public static string TypeOf(string line)
    {
        using var doc = JsonDocument.Parse(line);
        return doc.RootElement.TryGetProperty("type", out var t) ? t.GetString() ?? "" : "";
    }
}

/// <summary>Hands out a fresh <see cref="FakeShowEngineChild"/> per generation and records every spawn.</summary>
public sealed class FakeChildFactory : IShowEngineChildFactory
{
    private readonly List<FakeShowEngineChild> _children = new();
    private readonly List<ShowEngineSpawnRequest> _requests = new();
    private readonly object _gate = new();

    /// <summary>Preload each new child's stdout with an unsolicited handshake for its generation.</summary>
    public Func<int, bool> PreloadHandshake { get; set; } = _ => true;

    /// <summary>Protocol version the preloaded handshake declares.</summary>
    public int HandshakeProtocolVersion { get; set; } = 1;

    /// <summary>Applied to each fresh child before its reader loop starts.</summary>
    public Action<FakeShowEngineChild>? Configure { get; set; }

    public int SpawnCount { get { lock (_gate) { return _children.Count; } } }

    public IReadOnlyList<ShowEngineSpawnRequest> Requests { get { lock (_gate) { return _requests.ToArray(); } } }

    /// <summary>1-based: <c>Child(1)</c> is generation 1.</summary>
    public FakeShowEngineChild Child(int generation)
    {
        lock (_gate)
        {
            return _children.Single(c => c.Generation == generation);
        }
    }

    public IShowEngineChild Spawn(int generation, ShowEngineSpawnRequest request)
    {
        var child = new FakeShowEngineChild(generation);
        Configure?.Invoke(child);
        if (PreloadHandshake(generation))
        {
            child.Push(TestLines.HandshakeEvent(generation, HandshakeProtocolVersion));
        }

        lock (_gate)
        {
            _children.Add(child);
            _requests.Add(request);
        }

        return child;
    }
}
