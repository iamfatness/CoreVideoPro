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
    public int HandshakeRequestTimeoutMs { get; init; } = 15000;
    public int ZoomJoinRequestTimeoutMs { get; init; } = 60000;
    public int MaxRestarts { get; init; } = 5;
    public int FrameDrainIntervalMs { get; init; } = 16;
}

public sealed class MediaCoreSupervisor : IAsyncDisposable
{
    private readonly object _gate = new();
    private readonly SemaphoreSlim _stdinGate = new(1, 1);
    private readonly MediaCoreSupervisorOptions _options;
    private Process? _process;
    private StreamWriter? _stdin;
    private readonly Dictionary<string, TaskCompletionSource<JsonDocument>> _pending = new();
    private Timer? _frameDrainTimer;
    private int _nextId;
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

        var profile = await EnsureHandshakeProfileAsync(cancellationToken).ConfigureAwait(false);
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
        lock (_gate)
        {
            if (_profile is not null)
            {
                return _profile;
            }
        }

        var response = await SendAsync(
            new Dictionary<string, object?>
            {
                ["id"] = NextId(),
                ["type"] = "handshake"
            },
            cancellationToken,
            _options.HandshakeRequestTimeoutMs).ConfigureAwait(false);

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
        string? sdkJwt = null,
        string? userZak = null,
        CancellationToken cancellationToken = default)
    {
        var joinDetails = ZoomMeetingUrlParser.Parse(meetingUrl);
        var payload = new Dictionary<string, object?>
        {
            ["meetingUrl"] = joinDetails.MeetingUrl,
            ["displayName"] = displayName,
            ["webinar"] = webinar
        };
        if (!string.IsNullOrWhiteSpace(joinDetails.Passcode))
        {
            payload["passcode"] = joinDetails.Passcode;
        }

        if (!string.IsNullOrWhiteSpace(joinDetails.MeetingNumber))
        {
            payload["meetingNumber"] = joinDetails.MeetingNumber;
        }

        if (!string.IsNullOrWhiteSpace(sdkJwt))
        {
            payload["sdkJwt"] = sdkJwt;
        }

        if (!string.IsNullOrWhiteSpace(userZak))
        {
            payload["userZak"] = userZak;
        }

        var response = await SendAsync(
            new Dictionary<string, object?>
            {
                ["id"] = NextId(),
                ["type"] = "zoom-join",
                ["payload"] = payload
            },
            cancellationToken,
            _options.ZoomJoinRequestTimeoutMs).ConfigureAwait(false);

        using (response)
        {
            var snapshot = CoreProtocolParser.TryParseCaptureSnapshot(response, "zoom-join");
            if (snapshot is not null)
            {
                return snapshot;
            }

            throw new InvalidOperationException(
                CoreProtocolParser.DescribeUnexpectedCaptureResponse(response, "zoom-join"));
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
                CoreProtocolParser.DescribeUnexpectedCaptureResponse(response, "zoom-leave"));
        }
    }

    public async Task<ZoomMediaSpineNativeSnapshot> SyncZoomMediaSpineAsync(
        Dictionary<string, object?> spinePayload,
        double elapsedMs,
        CancellationToken cancellationToken = default)
    {
        var response = await SendAsync(
            new Dictionary<string, object?>
            {
                ["id"] = NextId(),
                ["type"] = "zoom-media-spine-sync",
                ["spinePayload"] = spinePayload,
                ["elapsedMs"] = elapsedMs
            },
            cancellationToken).ConfigureAwait(false);

        using (response)
        {
            var snapshot = CoreProtocolParser.TryParseZoomMediaSpineSnapshot(response);
            if (snapshot is not null)
            {
                return snapshot;
            }

            throw new InvalidOperationException(
                $"zoom-media-spine-sync failed: {CoreProtocolParser.TryParseErrorMessage(response)}");
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
        var env = new Dictionary<string, string>(StringComparer.Ordinal);
        foreach (var pair in MediaCorePaths.BuildMediaCoreChildEnvironment())
        {
            env[pair.Key] = pair.Value;
        }

        if (_options.Environment is not null)
        {
            foreach (var pair in _options.Environment)
            {
                env[pair.Key] = pair.Value;
            }
        }

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
                throw new InvalidOperationException(
                    "No native media core binary found. Run npm run test:native-media-core or scripts/build-studio.ps1.");
            }
        }

        var startInfo = new ProcessStartInfo
        {
            FileName = fileName,
            Arguments = arguments,
            WorkingDirectory = _options.WorkingDirectory ?? MediaCorePaths.ResolveMediaCoreWorkingDirectory(),
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

        var process = _process;
        _stdin = process.StandardInput;
        _ = Task.Run(() => ReadStdoutLoopAsync(process));
        _process.ErrorDataReceived += (_, e) =>
        {
            if (!string.IsNullOrWhiteSpace(e.Data))
            {
                Debug.WriteLine($"[media-core] {e.Data}");
            }
        };
        _process.BeginErrorReadLine();
    }

    private async Task ReadStdoutLoopAsync(Process process)
    {
        while (!process.HasExited)
        {
            string? line;
            try
            {
                line = await process.StandardOutput.ReadLineAsync().ConfigureAwait(false);
            }
            catch (ObjectDisposedException)
            {
                return;
            }
            catch (InvalidOperationException ex)
            {
                RejectAllForProcess(
                    process,
                    new InvalidOperationException("Media core stream reader was already active for this process.", ex));
                return;
            }

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

            if (!document.RootElement.TryGetProperty("id", out var idElement))
            {
                document.Dispose();
                continue;
            }

            var id = idElement.ValueKind switch
            {
                JsonValueKind.String => idElement.GetString(),
                JsonValueKind.Number => idElement.GetRawText(),
                _ => null
            };
            if (id is null)
            {
                document.Dispose();
                continue;
            }

            TaskCompletionSource<JsonDocument>? waiter = null;
            lock (_gate)
            {
                if (!ReferenceEquals(_process, process))
                {
                    document.Dispose();
                    continue;
                }

                if (_pending.Remove(id, out waiter))
                {
                    waiter.TrySetResult(document);
                    continue;
                }
            }

            if (TryAcceptBootstrapHandshake(document))
            {
                document.Dispose();
                continue;
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

    private async Task<NativeMediaCoreProfile?> EnsureHandshakeProfileAsync(CancellationToken cancellationToken)
    {
        var deadline = DateTime.UtcNow.AddMilliseconds(_options.HandshakeRequestTimeoutMs);
        while (DateTime.UtcNow < deadline)
        {
            cancellationToken.ThrowIfCancellationRequested();

            lock (_gate)
            {
                if (_profile is not null)
                {
                    return _profile;
                }

                if (_process is null || _process.HasExited)
                {
                    throw new InvalidOperationException(DescribeChildStartupFailure());
                }
            }

            await Task.Delay(25, cancellationToken).ConfigureAwait(false);
        }

        return await HandshakeAsync(cancellationToken).ConfigureAwait(false);
    }

    private string DescribeChildStartupFailure()
    {
        var exitCode = _process?.HasExited == true ? _process.ExitCode.ToString() : "running";
        return $"Media core exited before handshake completed (exit {exitCode}).";
    }

    private bool TryAcceptBootstrapHandshake(JsonDocument document)
    {
        if (!MediaCoreHandshakeRules.IsUnsolicitedBootstrapHandshake(document.RootElement))
        {
            return false;
        }

        var profile = CoreProtocolParser.TryParseHandshakeProfile(document);
        if (profile is null)
        {
            return false;
        }

        lock (_gate)
        {
            _profile = profile;
            _recovering = false;
        }

        ProfileChanged?.Invoke(profile);
        return true;
    }

    private async Task<JsonDocument> SendAsync(
        Dictionary<string, object?> payload,
        CancellationToken cancellationToken,
        int? timeoutMs = null)
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
        timeoutCts.CancelAfter(timeoutMs ?? _options.RequestTimeoutMs);
        await using var registration = timeoutCts.Token.Register(() =>
        {
            lock (_gate)
            {
                _pending.Remove(id);
            }

            tcs.TrySetException(new TimeoutException($"media core request {id} timed out."));
        });

        var json = JsonSerializer.Serialize(payload, MediaCoreJson.Options);
        await _stdinGate.WaitAsync(cancellationToken).ConfigureAwait(false);
        try
        {
            try
            {
                StreamWriter? stdin;
                lock (_gate)
                {
                    stdin = _stdin;
                }

                if (stdin is null)
                {
                    throw new InvalidOperationException("Media core is not running.");
                }

                await stdin.WriteLineAsync(json).ConfigureAwait(false);
                await stdin.FlushAsync().ConfigureAwait(false);
            }
            catch (Exception ex) when (ex is InvalidOperationException or IOException or ObjectDisposedException)
            {
                lock (_gate)
                {
                    _pending.Remove(id);
                }

                tcs.TrySetException(ex);
                throw;
            }
        }
        finally
        {
            _stdinGate.Release();
        }

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

        var stdin = _stdin;
        var process = _process;
        _stdin = null;
        _process = null;

        try
        {
            stdin?.Close();
        }
        catch
        {
            // Best effort.
        }

        if (process is { HasExited: false })
        {
            try
            {
                process.Kill(entireProcessTree: true);
            }
            catch
            {
                // Best effort.
            }

            try
            {
                process.WaitForExit(1500);
            }
            catch
            {
                // Best effort.
            }
        }

        process?.Dispose();
    }

    private void RejectAll(Exception error)
    {
        foreach (var pending in _pending.Values)
        {
            pending.TrySetException(error);
        }

        _pending.Clear();
    }

    private void RejectAllForProcess(Process process, Exception error)
    {
        lock (_gate)
        {
            if (ReferenceEquals(_process, process))
            {
                RejectAll(error);
            }
        }
    }

    private string NextId() => $"core-{Interlocked.Increment(ref _nextId)}";

    private void RaiseHealth() => HealthChanged?.Invoke(Health);

    public ValueTask DisposeAsync()
    {
        Stop();
        return ValueTask.CompletedTask;
    }
}
