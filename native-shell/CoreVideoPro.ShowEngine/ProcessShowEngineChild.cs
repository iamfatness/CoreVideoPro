using System.Diagnostics;
using System.Text;

namespace CoreVideoPro.ShowEngine;

/// <summary>
/// The real <see cref="IShowEngineChild"/>: one <c>node.exe dist/host/main.js --config &lt;path&gt;
/// --generation &lt;n&gt;</c> process, per spec §6.3. Deliberately THIN — it owns no policy, only the
/// process. Every decision (timeouts, correlation, restart, generation guards) lives in
/// <see cref="ShowEngineSupervisor"/>, which is where the tests are; this class cannot be unit-tested
/// without Node on the box, so there must be nothing in it worth testing.
/// </summary>
public sealed class ProcessShowEngineChild : IShowEngineChild
{
    /// <summary>UTF-8 without a BOM on all three streams: a BOM on stdin would be the first bytes the
    /// host's line reader sees and would make its very first JSON.parse fail.</summary>
    private static readonly UTF8Encoding ChildProcessEncoding = new(encoderShouldEmitUTF8Identifier: false);

    private readonly Process _process;
    private readonly ShowEngineLog _log;
    private readonly TaskCompletionSource<int> _exited =
        new(TaskCreationOptions.RunContinuationsAsynchronously);

    public ProcessShowEngineChild(int generation, ShowEngineSpawnRequest request, ShowEngineLog log)
    {
        Generation = generation;
        _log = log;

        var startInfo = new ProcessStartInfo
        {
            FileName = request.NodeExe,
            WorkingDirectory = request.WorkingDirectory,
            UseShellExecute = false,
            RedirectStandardInput = true,
            RedirectStandardOutput = true,
            RedirectStandardError = true,
            StandardInputEncoding = ChildProcessEncoding,
            StandardOutputEncoding = ChildProcessEncoding,
            StandardErrorEncoding = ChildProcessEncoding,
            CreateNoWindow = true
        };

        startInfo.ArgumentList.Add(request.EntryScript);
        startInfo.ArgumentList.Add("--config");
        startInfo.ArgumentList.Add(request.ConfigPath);
        startInfo.ArgumentList.Add("--generation");
        startInfo.ArgumentList.Add(generation.ToString());

        foreach (var pair in request.Environment) startInfo.Environment[pair.Key] = pair.Value;

        _process = new Process { StartInfo = startInfo, EnableRaisingEvents = true };
        _process.Exited += (_, _) =>
        {
            var code = TryExitCode();
            _log.Append($"[show-engine] gen {generation} exited with code {code}");
            _exited.TrySetResult(code);
        };
        _process.ErrorDataReceived += (_, e) =>
        {
            if (e.Data is not null) _log.Append($"[show-engine gen {generation}] {e.Data}");
        };

        _log.Append($"[show-engine] spawn gen {generation}: {request.NodeExe} {request.EntryScript} " +
                    $"--config {request.ConfigPath}");

        if (!_process.Start())
        {
            throw new InvalidOperationException("Failed to start the OHG show engine process.");
        }

        _process.BeginErrorReadLine();
    }

    public int Generation { get; }

    public async Task WriteLineAsync(string line, CancellationToken ct)
    {
        await _process.StandardInput.WriteLineAsync(line.AsMemory(), ct).ConfigureAwait(false);
        await _process.StandardInput.FlushAsync(ct).ConfigureAwait(false);
    }

    public Task<string?> ReadLineAsync(CancellationToken ct) =>
        _process.StandardOutput.ReadLineAsync(ct).AsTask();

    public Task<int> Exited => _exited.Task;

    public void Kill()
    {
        try
        {
            if (!_process.HasExited) _process.Kill(entireProcessTree: true);
        }
        catch
        {
            // Best effort — the process may have died between the check and the call.
        }
    }

    public void Dispose()
    {
        try { _process.StandardInput.Close(); } catch { /* best effort */ }
        Kill();
        try { _process.WaitForExit(1500); } catch { /* best effort */ }
        _exited.TrySetResult(TryExitCode());
        _process.Dispose();
    }

    private int TryExitCode()
    {
        try { return _process.HasExited ? _process.ExitCode : -1; }
        catch { return -1; }
    }
}

public sealed class ProcessShowEngineChildFactory : IShowEngineChildFactory
{
    private readonly ShowEngineLog _log;

    public ProcessShowEngineChildFactory(ShowEngineLog? log = null) => _log = log ?? new ShowEngineLog();

    public IShowEngineChild Spawn(int generation, ShowEngineSpawnRequest request) =>
        new ProcessShowEngineChild(generation, request, _log);
}
