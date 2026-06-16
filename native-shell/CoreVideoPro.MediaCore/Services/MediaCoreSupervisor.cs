using System.Diagnostics;
using System.Text.Json;
using CoreVideoPro.MediaCore.Json;
using CoreVideoPro.MediaCore.Models;

namespace CoreVideoPro.MediaCore.Services;

public sealed class MediaCoreSupervisorOptions
{
    public string? Command { get; init; }
    public IReadOnlyList<string>? Args { get; init; }
    public string? WorkingDirectory { get; init; }
    public IReadOnlyDictionary<string, string>? Environment { get; init; }
    public int RequestTimeoutMs { get; init; } = 4000;
    public int MaxRestarts { get; init; } = 5;
    public int FrameDrainIntervalMs { get; init; } = 16;
}

public sealed class MediaCoreSupervisor : IAsyncDisposable
{
    private readonly object _gate = new();
    private readonly MediaCoreSupervisorOptions _options;
    private Process? _process;
    private StreamWriter? _stdin;
    private readonly Dictionary<string, TaskCompletionSource<JsonDocument>> _pending = new();
    private Timer? _frameDrainTimer;
    private int _nextId = 1;
    private int _restarts;
    private bool _stopped = true;
    private bool _recovering;
    private bool _syncInFlight;
    private int _syncFrameNumber;
    private NativeMediaCoreProfile? _profile;

    public MediaCoreSupervisor(MediaCoreSupervisorOptions? options = null)
    {
        _options = options ?? new MediaCoreSupervisorOptions();
    }

    public event Action<MediaCoreHealth>? HealthChanged;
    public event Action<string>? StatusChanged;
    public event Action<NativeMediaCoreProfile>? ProfileChanged;
    public event Action<ZoomVideoFrame>? ZoomVideoFrameReceived;
    public event Action<ProgramFramePreview>? ProgramFramePreviewReceived;
    public event Action<ProgramSharedTexture>? ProgramSharedTextureReceived;

    public MediaCoreHealth Health => new()
    {
        RestartCount = _restarts,
        Recovering = _recovering,
        Stopped = _stopped
    };

    public bool Running
    {
        get
        {
            lock (_gate)
            {
                return !_stopped && _process is { HasExited: false };
            }
        }
    }

    public NativeMediaCoreProfile? Profile
    {
        get
        {
            lock (_gate)
            {
                return _profile;
            }
        }
    }

    public string ProfileSummary => Profile?.Name ?? "Engine off";

    public async Task<NativeMediaCoreProfile?> StartAsync(CancellationToken cancellationToken = default)
    {
        lock (_gate)
        {
            if (!_stopped && _process is { HasExited: false })
            {
                return _profile;
            }

            _stopped = false;
            SpawnChild();
        }

        var profile = await HandshakeAsync(cancellationToken).ConfigureAwait(false);
        StartFrameDrain();
        RaiseHealth();
        StatusChanged?.Invoke("Engine on");
        return profile;
    }

    public void Stop()
    {
        lock (_gate)
        {
            _stopped = true;
            _recovering = false;
            StopFrameDrain();
            TeardownChild();
            _profile = null;
        }

        RaiseHealth();
        StatusChanged?.Invoke("Engine off — Zoom ingest paused");
    }

    public async Task<bool> PingAsync(CancellationToken cancellationToken = default)
    {
        var response = await SendAsync(
            new Dictionary<string, object?>
            {
                ["id"] = NextId(),
                ["type"] = "ping"
            },
            cancellationToken).ConfigureAwait(false);

        using (response)
        {
            return response.RootElement.TryGetProperty("ok", out var okElement) && okElement.GetBoolean();
        }
    }

    public async Task<NativeMediaCoreProfile?> HandshakeAsync(CancellationToken cancellationToken = default)
    {
        var response = await SendAsync(
            new Dictionary<string, object?>
            {
                ["id"] = NextId(),
                ["type"] = "handshake"
            },
            cancellationToken).ConfigureAwait(false);

        using (response)
        {
            var profile = CoreProtocolParser.TryParseHandshakeProfile(response);
            if (profile is not null)
            {
                lock (_gate)
                {
                    _profile = profile;
                    _recovering = false;
                }

                ProfileChanged?.Invoke(profile);
            }

            return profile;
        }
    }

    public async Task<RawCaptureSnapshot> JoinZoomAsync(
        string meetingUrl,
        string displayName,
        bool webinar,
        CancellationToken cancellationToken = default)
    {
        var response = await SendAsync(
            new Dictionary<string, object?>
            {
                ["id"] = NextId(),
                ["type"] = "zoom-join",
                ["payload"] = new Dictionary<string, object?>
                {
                    ["meetingUrl"] = meetingUrl,
                    ["displayName"] = displayName,
                    ["webinar"] = webinar
                }
            },
            cancellationToken).ConfigureAwait(false);

        using (response)
        {
            var snapshot = CoreProtocolParser.TryParseCaptureSnapshot(response, "zoom-join");
            if (snapshot is not null)
            {
                return snapshot;
            }

            throw new InvalidOperationException(
                $"zoom join failed: {CoreProtocolParser.TryParseErrorMessage(response)}");
        }
    }

    public async Task<RawCaptureSnapshot> LeaveZoomAsync(CancellationToken cancellationToken = default)
    {
        var response = await SendAsync(
            new Dictionary<string, object?>
            {
                ["id"] = NextId(),
                ["type"] = "zoom-leave"
            },
            cancellationToken).ConfigureAwait(false);

        using (response)
        {
            var snapshot = CoreProtocolParser.TryParseCaptureSnapshot(response, "zoom-leave");
            if (snapshot is not null)
            {
                return snapshot;
            }

            throw new InvalidOperationException(
                $"zoom leave failed: {CoreProtocolParser.TryParseErrorMessage(response)}");
        }
    }

    public async Task<RawCaptureSnapshot> GetZoomSnapshotAsync(CancellationToken cancellationToken = default)
    {
        var response = await SendAsync(
            new Dictionary<string, object?>
            {
                ["id"] = NextId(),
                ["type"] = "zoom-snapshot"
            },
            cancellationToken).ConfigureAwait(false);

        using (response)
        {
            var snapshot = CoreProtocolParser.TryParseCaptureSnapshot(response, "zoom-snapshot");
            if (snapshot is not null)
            {
                return snapshot;
            }

            throw new InvalidOperationException(
                $"zoom snapshot failed: {CoreProtocolParser.TryParseErrorMessage(response)}");
        }
    }

    public async Task<NativeMediaCoreStateSnapshot> SyncMediaCoreAsync(
        IReadOnlyList<NativeMediaCoreCommand> commands,
        double elapsedMs,
        CancellationToken cancellationToken = default)
    {
        if (_syncInFlight)
        {
            throw new InvalidOperationException("media-core sync in flight; skipped for backpressure");
        }

        _syncInFlight = true;
        try
        {
            var response = await SendAsync(
                new Dictionary<string, object?>
                {
                    ["id"] = NextId(),
                    ["type"] = "media-core-sync",
                    ["commands"] = commands,
                    ["elapsedMs"] = elapsedMs
                },
                cancellationToken).ConfigureAwait(false);

            using (response)
            {
                if (response.RootElement.TryGetProperty("ok", out var okElement) && !okElement.GetBoolean())
                {
                    throw new InvalidOperationException(
                        $"media-core sync failed: {CoreProtocolParser.TryParseErrorMessage(response)}");
                }

                var snapshot = CoreProtocolParser.TryParseSyncSnapshot(response);
                if (snapshot is not null &&
                    !IsWireSnapshotResponse(response))
                {
                    return snapshot;
                }

                var wire = CoreProtocolParser.TryParseWireState(response);
                if (wire is not null)
                {
                    _syncFrameNumber += 1;
                    return NativeMediaCoreStateMapper.MapNativeWireStateToSnapshot(
                        commands,
                        elapsedMs,
                        _syncFrameNumber,
                        wire);
                }

                throw new InvalidOperationException("media-core sync failed: Unexpected response type.");
            }
        }
        finally
        {
            _syncInFlight = false;
        }
    }

    private static bool IsWireSnapshotResponse(JsonDocument response)
    {
        var root = response.RootElement;
        if (!root.TryGetProperty("snapshot", out var snapshotElement) &&
            !root.TryGetProperty("state", out snapshotElement))
        {
            return false;
        }

        return NativeMediaCoreStateMapper.IsNativeMediaCoreWireState(snapshotElement);
    }

    private void SpawnChild()
    {
        string fileName;
        string arguments;
        var env = new Dictionary<string, string>(_options.Environment ?? new Dictionary<string, string>());

        if (_options.Command is not null)
        {
            fileName = _options.Command;
            arguments = _options.Args is { Count: > 0 }
                ? string.Join(' ', _options.Args.Select(arg => $"\"{arg}\""))
                : string.Empty;
        }
        else
        {
            var nativeExe = MediaCorePaths.ResolveNativeCoreExecutable();
            if (nativeExe is not null)
            {
                fileName = nativeExe;
                arguments = string.Empty;
            }
            else
            {
                var node = MediaCorePaths.ResolveNodeCoreStub()
                           ?? throw new InvalidOperationException(
                               "No media core binary or Node stub found. Build native/ or run npm install.");
                var stub = Path.Combine(MediaCorePaths.RepoRoot, "desktop", "coreStub.cjs");
                fileName = node;
                arguments = $"\"{stub}\"";
                if (node.EndsWith("electron.exe", StringComparison.OrdinalIgnoreCase))
                {
                    env["ELECTRON_RUN_AS_NODE"] = "1";
                }
            }
        }

        var startInfo = new ProcessStartInfo
        {
            FileName = fileName,
            Arguments = arguments,
            WorkingDirectory = _options.WorkingDirectory ?? MediaCorePaths.RepoRoot,
            UseShellExecute = false,
            RedirectStandardInput = true,
            RedirectStandardOutput = true,
            RedirectStandardError = true,
            CreateNoWindow = true
        };

        foreach (var pair in env)
        {
            startInfo.Environment[pair.Key] = pair.Value;
        }

        _process = new Process { StartInfo = startInfo, EnableRaisingEvents = true };
        _process.Exited += OnChildExited;
        if (!_process.Start())
        {
            throw new InvalidOperationException("Failed to start media core process.");
        }

        _stdin = _process.StandardInput;
        _ = Task.Run(ReadStdoutLoopAsync);
        _process.BeginErrorReadLine();
        _process.ErrorDataReceived += (_, e) =>
        {
            if (!string.IsNullOrWhiteSpace(e.Data))
            {
                Debug.WriteLine($"[media-core] {e.Data}");
            }
        };
    }

    private async Task ReadStdoutLoopAsync()
    {
        if (_process?.StandardOutput is null)
        {
            return;
        }

        while (!_process.HasExited)
        {
            var line = await _process.StandardOutput.ReadLineAsync().ConfigureAwait(false);
            if (line is null)
            {
                break;
            }

            if (line.Trim().Length == 0)
            {
                continue;
            }

            var frameEvent = CoreProtocolParser.TryParseEvent(line);
            if (frameEvent is not null)
            {
                ZoomVideoFrameReceived?.Invoke(frameEvent.Frame);
                continue;
            }

            var previewEvent = CoreProtocolParser.TryParseProgramFramePreviewEvent(line);
            if (previewEvent is not null)
            {
                ProgramFramePreviewReceived?.Invoke(previewEvent.Preview);
                continue;
            }

            var sharedTextureEvent = CoreProtocolParser.TryParseProgramSharedTextureEvent(line);
            if (sharedTextureEvent is not null)
            {
                ProgramSharedTextureReceived?.Invoke(sharedTextureEvent.Texture);
                continue;
            }

            JsonDocument? document = null;
            try
            {
                document = JsonDocument.Parse(line);
            }
            catch (JsonException)
            {
                continue;
            }

            if (!document.RootElement.TryGetProperty("id", out var idElement) ||
                idElement.ValueKind != JsonValueKind.String)
            {
                document.Dispose();
                continue;
            }

            var id = idElement.GetString();
            if (id is null)
            {
                document.Dispose();
                continue;
            }

            TaskCompletionSource<JsonDocument>? waiter = null;
            lock (_gate)
            {
                if (_pending.Remove(id, out waiter))
                {
                    waiter.TrySetResult(document);
                    continue;
                }
            }

            document.Dispose();
        }
    }

    private void OnChildExited(object? sender, EventArgs e)
    {
        lock (_gate)
        {
            RejectAll(new InvalidOperationException("Media core exited."));
            if (_stopped)
            {
                return;
            }

            _restarts++;
            _recovering = true;
            RaiseHealth();
            StatusChanged?.Invoke($"Media core recovering (restart {_restarts})");

            if (_restarts > _options.MaxRestarts)
            {
                _process = null;
                StatusChanged?.Invoke("Media core failed after repeated restarts.");
                return;
            }

            SpawnChild();
        }

        _ = HandshakeAsync(CancellationToken.None);
    }

    private async Task<JsonDocument> SendAsync(
        Dictionary<string, object?> payload,
        CancellationToken cancellationToken)
    {
        if (!payload.TryGetValue("id", out var idValue) || idValue is not string id)
        {
            throw new InvalidOperationException("Request id missing.");
        }

        var tcs = new TaskCompletionSource<JsonDocument>(TaskCreationOptions.RunContinuationsAsynchronously);
        lock (_gate)
        {
            if (_stdin is null || _process is null || _process.HasExited)
            {
                throw new InvalidOperationException("Media core is not running.");
            }

            _pending[id] = tcs;
        }

        using var timeoutCts = CancellationTokenSource.CreateLinkedTokenSource(cancellationToken);
        timeoutCts.CancelAfter(_options.RequestTimeoutMs);
        await using var registration = timeoutCts.Token.Register(() =>
        {
            lock (_gate)
            {
                _pending.Remove(id);
            }

            tcs.TrySetException(new TimeoutException($"media core request {id} timed out."));
        });

        var json = JsonSerializer.Serialize(payload, MediaCoreJson.Options);
        await _stdin.WriteLineAsync(json).ConfigureAwait(false);
        await _stdin.FlushAsync().ConfigureAwait(false);
        return await tcs.Task.ConfigureAwait(false);
    }

    private void StartFrameDrain()
    {
        StopFrameDrain();
        var interval = Math.Max(1, _options.FrameDrainIntervalMs);
        _frameDrainTimer = new Timer(
            _ =>
            {
                if (!Running)
                {
                    return;
                }

                _ = PingAsync().ContinueWith(
                    task => task.Exception,
                    CancellationToken.None,
                    TaskContinuationOptions.OnlyOnFaulted,
                    TaskScheduler.Default);
            },
            null,
            interval,
            interval);
    }

    private void StopFrameDrain()
    {
        _frameDrainTimer?.Dispose();
        _frameDrainTimer = null;
    }

    private void TeardownChild()
    {
        RejectAll(new InvalidOperationException("Supervisor stopped."));
        if (_process is { HasExited: false })
        {
            try
            {
                _process.Kill(entireProcessTree: true);
            }
            catch
            {
                // Best effort.
            }
        }

        _process?.Dispose();
        _process = null;
        _stdin = null;
    }

    private void RejectAll(Exception error)
    {
        foreach (var pending in _pending.Values)
        {
            pending.TrySetException(error);
        }

        _pending.Clear();
    }

    private string NextId() => $"core-{_nextId++}";

    private void RaiseHealth() => HealthChanged?.Invoke(Health);

    public ValueTask DisposeAsync()
    {
        Stop();
        return ValueTask.CompletedTask;
    }
}