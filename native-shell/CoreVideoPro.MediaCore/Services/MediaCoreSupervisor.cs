using System.Diagnostics;
using System.Text;
using System.Text.Json;
using System.Threading.Channels;
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
    public int ZoomRecoveryMaxAttempts { get; init; } = 3;
    public int ZoomRecoveryRetryDelayMs { get; init; } = 2500;
    public int MaxRestarts { get; init; } = 5;
    // Phase 2 / Increment 1: the "frame drain" ping is a pure liveness heartbeat —
    // video/preview/texture frames pump autonomously on the core's own threads
    // (render thread, zoom pump), so this timer never moves a frame. At the old
    // 16ms cadence it fired ~62/s, and every ping still acquired the core's command
    // mutex (serialized behind real syncs and contending the render thread). Slowed
    // to 1s: ~62/s of pure lock contention removed, a heartbeat retained for liveness.
    public int FrameDrainIntervalMs { get; init; } = 1000;
}

public sealed class MediaCoreSupervisor : IAsyncDisposable
{
    internal static Encoding ChildProcessEncoding { get; } =
        new UTF8Encoding(encoderShouldEmitUTF8Identifier: false);

    private readonly object _gate = new();
    private readonly SemaphoreSlim _stdinGate = new(1, 1);
    private readonly MediaCoreSupervisorOptions _options;
    private Process? _process;
    private StreamWriter? _stdin;
    private readonly Dictionary<string, TaskCompletionSource<JsonDocument>> _pending = new();
    private Timer? _frameDrainTimer;
    // Frame/preview/texture handlers run here (off the stdout read loop) so their
    // UI-thread marshaling can never stall reading — which would back up stdout and
    // time out every command response. Drop-oldest: preview is latest-wins.
    private Channel<Action>? _frameDispatch;
    private long _zoomFrameCounter;

    private static void PerfLog(string message)
    {
        BoundedLogFile.Append(
            System.IO.Path.Combine(
                Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData),
                "CoreVideoPro", "perf.log"),
            $"[{DateTimeOffset.Now:HH:mm:ss.fff}] {message}{Environment.NewLine}");
    }
    private int _nextId;
    private int _restarts;
    private const int MaxCrashEvents = 20;
    private readonly LinkedList<MediaCoreCrashEvent> _crashEvents = new();
    private bool _stopped = true;
    private bool _recovering;
    private bool _syncInFlight;
    private int _syncFrameNumber;
    private NativeMediaCoreProfile? _profile;
    private Dictionary<string, object?>? _zoomJoinRecoveryPayload;
    private bool _zoomRawCapturePaused;

    public MediaCoreSupervisor(MediaCoreSupervisorOptions? options = null)
    {
        _options = options ?? new MediaCoreSupervisorOptions();
    }

    public event Action<MediaCoreHealth>? HealthChanged;
    public event Action<string>? StatusChanged;
    public event Action<NativeMediaCoreProfile>? ProfileChanged;
    public event Action<RawCaptureSnapshot>? ZoomRecovered;
    public event Action<ZoomVideoFrame>? ZoomVideoFrameReceived;
    public event Action<ProgramFramePreview>? ProgramFramePreviewReceived;
    public event Action<ProgramSharedTexture>? ProgramSharedTextureReceived;
    public event Action<ProgramSharedTexture>? PreviewSharedTextureReceived;
    public event Action<ParticipantSharedTexture>? ParticipantSharedTextureReceived;
    public event Action<MultiviewSharedTexture>? MultiviewSharedTextureReceived;

    public MediaCoreHealth Health
    {
        get
        {
            lock (_gate)
            {
                return new MediaCoreHealth
                {
                    RestartCount = _restarts,
                    Recovering = _recovering,
                    Stopped = _stopped,
                    CrashEvents = _crashEvents.ToArray()
                };
            }
        }
    }

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
            _zoomJoinRecoveryPayload = null;
            _zoomRawCapturePaused = false;
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

    public async Task OpenVstEditorAsync(string selection, CancellationToken cancellationToken = default)
    {
        var response = await SendAsync(
            new Dictionary<string, object?>
            {
                ["id"] = NextId(),
                ["type"] = "open-vst-editor",
                ["selection"] = selection
            },
            cancellationToken).ConfigureAwait(false);
        using (response)
        {
            // A1 regression guard: this response used to be discarded unread,
            // which made a core-side rejection (e.g. the pre-fix protocol-error
            // for a top-level open-vst-editor request) a SILENT no-op — the
            // owner-reported "Open controls never shows a plugin UI" defect.
            // Any ok:false must surface to the operator as status text.
            if (response.RootElement.TryGetProperty("ok", out var okElement) &&
                okElement.ValueKind == JsonValueKind.False)
            {
                var message =
                    response.RootElement.TryGetProperty("error", out var errorElement) &&
                    errorElement.ValueKind == JsonValueKind.Object &&
                    errorElement.TryGetProperty("message", out var messageElement)
                        ? messageElement.GetString()
                        : null;
                throw new InvalidOperationException(
                    message ?? "the native media core rejected the open-vst-editor command");
            }
        }
    }

    // A2: generic VST param slider -> core -> isolated host. Fire-and-forget
    // semantics with a loud failure: an ok:false response surfaces as an
    // exception the view model turns into status text.
    public async Task SetVstParamAsync(string selection, long paramId, double normalized,
        CancellationToken cancellationToken = default)
    {
        var response = await SendAsync(
            new Dictionary<string, object?>
            {
                ["id"] = NextId(),
                ["type"] = "set-vst-param",
                ["selection"] = selection,
                ["paramId"] = paramId,
                ["normalized"] = normalized
            },
            cancellationToken).ConfigureAwait(false);
        using (response)
        {
            ThrowIfRejected(response, "set-vst-param");
        }
    }

    // A2: push a saved component-state blob (base64). The core caches it per
    // selection and injects it into every isolated-host generation — including
    // after a respawn — so plugin state survives restarts and host crashes.
    public async Task SetVstStateAsync(string selection, string stateBase64,
        CancellationToken cancellationToken = default)
    {
        var response = await SendAsync(
            new Dictionary<string, object?>
            {
                ["id"] = NextId(),
                ["type"] = "set-vst-state",
                ["selection"] = selection,
                ["stateBase64"] = stateBase64
            },
            cancellationToken).ConfigureAwait(false);
        using (response)
        {
            ThrowIfRejected(response, "set-vst-state");
        }
    }

    // A2: pull the plugin's CURRENT component state (base64) from the isolated
    // host. Returns null when the core reports a loud error (host down, no
    // selection) — the caller keeps the previously persisted state.
    public async Task<string?> GetVstStateAsync(string selection, CancellationToken cancellationToken = default)
    {
        var response = await SendAsync(
            new Dictionary<string, object?>
            {
                ["id"] = NextId(),
                ["type"] = "get-vst-state",
                ["selection"] = selection
            },
            cancellationToken).ConfigureAwait(false);
        using (response)
        {
            if (response.RootElement.TryGetProperty("ok", out var okElement) &&
                okElement.ValueKind == JsonValueKind.False)
            {
                return null;
            }

            return response.RootElement.TryGetProperty("state", out var stateElement) &&
                   stateElement.ValueKind == JsonValueKind.Object &&
                   stateElement.TryGetProperty("stateBase64", out var blobElement)
                ? blobElement.GetString()
                : null;
        }
    }

    private static void ThrowIfRejected(JsonDocument response, string commandName)
    {
        if (response.RootElement.TryGetProperty("ok", out var okElement) &&
            okElement.ValueKind == JsonValueKind.False)
        {
            var message =
                response.RootElement.TryGetProperty("error", out var errorElement) &&
                errorElement.ValueKind == JsonValueKind.Object &&
                errorElement.TryGetProperty("message", out var messageElement)
                    ? messageElement.GetString()
                    : null;
            throw new InvalidOperationException(
                message ?? $"the native media core rejected the {commandName} command");
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
                if (ZoomMediaSpineSnapshotMerger.NormalizeMeetingState(snapshot.MeetingState) == "in_meeting")
                {
                    lock (_gate)
                    {
                        _zoomJoinRecoveryPayload = new Dictionary<string, object?>(payload, StringComparer.Ordinal);
                        // A successful join can precede Zoom granting raw-recording
                        // privilege by a few hundred milliseconds. That transient
                        // false is not an operator pause; only StopZoomCaptureAsync
                        // is allowed to set the recovery pause intent.
                        _zoomRawCapturePaused = false;
                    }
                }

                return snapshot;
            }

            throw new InvalidOperationException(
                CoreProtocolParser.DescribeUnexpectedCaptureResponse(response, "zoom-join"));
        }
    }

    public async Task<RawCaptureSnapshot> LeaveZoomAsync(CancellationToken cancellationToken = default)
    {
        // Clear the recovery intent before sending the leave. If the core dies during
        // this request, the operator's explicit leave must win over auto-rejoin.
        lock (_gate)
        {
            _zoomJoinRecoveryPayload = null;
            _zoomRawCapturePaused = false;
        }

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

    /// <summary>
    /// Capture-off: asks the core to stop Zoom raw media (zoom-stop-capture →
    /// engine stop_raw_media: StopRawRecording clears the participant-facing
    /// recording indicator + unsubscribe_all stops frames) while STAYING in the
    /// meeting. The returned snapshot carries the engine-reported
    /// <see cref="RawCaptureSnapshot.RawMediaActive"/>.
    /// </summary>
    public async Task<RawCaptureSnapshot> StopZoomCaptureAsync(CancellationToken cancellationToken = default)
    {
        var response = await SendAsync(
            new Dictionary<string, object?>
            {
                ["id"] = NextId(),
                ["type"] = "zoom-stop-capture"
            },
            cancellationToken).ConfigureAwait(false);

        using (response)
        {
            var snapshot = CoreProtocolParser.TryParseCaptureSnapshot(response, "zoom-stop-capture");
            if (snapshot is not null)
            {
                lock (_gate)
                {
                    _zoomRawCapturePaused = snapshot.RawMediaActive != true;
                }

                return snapshot;
            }

            throw new InvalidOperationException(
                CoreProtocolParser.DescribeUnexpectedCaptureResponse(response, "zoom-stop-capture"));
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
            throw new MediaCoreSyncInFlightException();
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

    public async Task SetCaptureAudioSyncOffsetAsync(
        string deviceId,
        int offsetMs,
        CancellationToken cancellationToken = default)
    {
        var response = await SendAsync(
            new Dictionary<string, object?>
            {
                ["id"] = NextId(),
                ["type"] = "set-capture-audio-sync-offset",
                ["payload"] = new Dictionary<string, object?>
                {
                    ["deviceId"] = deviceId,
                    ["offsetMs"] = offsetMs
                }
            },
            cancellationToken).ConfigureAwait(false);

        using (response)
        {
            if (response.RootElement.TryGetProperty("ok", out var okElement) && okElement.GetBoolean())
            {
                return;
            }

            throw new InvalidOperationException(
                $"capture audio sync failed: {CoreProtocolParser.TryParseErrorMessage(response)}");
        }
    }

    // Lists the capture devices the core itself enumerates (native UVC via
    // Media Foundation, DeckLink/AJA probes, stub devices). Returns [] when the
    // response is not a capture-devices payload.
    public async Task<IReadOnlyList<NativeCaptureDeviceStatus>> ListCaptureDevicesAsync(
        CancellationToken cancellationToken = default)
    {
        var response = await SendAsync(
            new Dictionary<string, object?>
            {
                ["id"] = NextId(),
                ["type"] = "list-capture-devices"
            },
            cancellationToken).ConfigureAwait(false);

        using (response)
        {
            return CoreProtocolParser.TryParseCaptureDevices(response) ?? [];
        }
    }

    // Asks the core to open a capture device with its own native adapter (the
    // Media Foundation UVC path). The returned list reflects the core's device
    // states after the attempt; callers decide success by finding the device
    // connected (see NativeUvcCapturePolicy.FindConnectedDevice) and fall back
    // to the WinUI MediaCapture bridge otherwise.
    public async Task<IReadOnlyList<NativeCaptureDeviceStatus>> ConnectCaptureDeviceAsync(
        string deviceId,
        CancellationToken cancellationToken = default,
        string? outputSourceId = null)
    {
        var payload = new Dictionary<string, object?>
        {
            ["deviceId"] = deviceId
        };
        // For native UVC cameras the core enumerates a DIFFERENT stable id than the
        // shell (Media Foundation vs WinRT symbolic link). Tell the core which id to
        // key the emitted frames by so they match the shell's `capture:<id>` routing.
        if (!string.IsNullOrEmpty(outputSourceId))
        {
            payload["outputSourceId"] = outputSourceId;
        }

        var response = await SendAsync(
            new Dictionary<string, object?>
            {
                ["id"] = NextId(),
                ["type"] = "connect-capture-device",
                ["payload"] = payload
            },
            cancellationToken).ConfigureAwait(false);

        using (response)
        {
            return CoreProtocolParser.TryParseCaptureDevices(response) ?? [];
        }
    }

    // Lifecycle L2: stop the device session core-side (WGC/SRT teardown).
    public async Task<IReadOnlyList<NativeCaptureDeviceStatus>> DisconnectCaptureDeviceAsync(
        string deviceId,
        CancellationToken cancellationToken = default)
    {
        var response = await SendAsync(
            new Dictionary<string, object?>
            {
                ["id"] = NextId(),
                ["type"] = "disconnect-capture-device",
                ["payload"] = new Dictionary<string, object?>
                {
                    ["deviceId"] = deviceId
                }
            },
            cancellationToken).ConfigureAwait(false);

        using (response)
        {
            return CoreProtocolParser.TryParseCaptureDevices(response) ?? [];
        }
    }

    // Browser sources (BR-1): the core spawns/supervises one corevideo-browser-host
    // (WebView2) process per source; frames enter the compositor keyed
    // "capture:browser:<n>". Failures come back as thrown errors — LOUD, never silent.
    public async Task AddBrowserSourceAsync(
        string url,
        int width,
        int height,
        int fps,
        CancellationToken cancellationToken = default)
    {
        var response = await SendAsync(
            new Dictionary<string, object?>
            {
                ["id"] = NextId(),
                ["type"] = "browser-add",
                ["payload"] = new Dictionary<string, object?>
                {
                    ["url"] = url,
                    ["width"] = width,
                    ["height"] = height,
                    ["fps"] = fps
                }
            },
            cancellationToken).ConfigureAwait(false);
        using (response)
        {
            ThrowIfNotOk(response, "browser-add");
        }
    }

    public async Task RemoveBrowserSourceAsync(string browserId, CancellationToken cancellationToken = default)
    {
        var response = await SendAsync(
            new Dictionary<string, object?>
            {
                ["id"] = NextId(),
                ["type"] = "browser-remove",
                ["payload"] = new Dictionary<string, object?> { ["browserId"] = browserId }
            },
            cancellationToken).ConfigureAwait(false);
        using (response)
        {
            ThrowIfNotOk(response, "browser-remove");
        }
    }

    public async Task ReloadBrowserSourceAsync(string browserId, CancellationToken cancellationToken = default)
    {
        var response = await SendAsync(
            new Dictionary<string, object?>
            {
                ["id"] = NextId(),
                ["type"] = "browser-reload",
                ["payload"] = new Dictionary<string, object?> { ["browserId"] = browserId }
            },
            cancellationToken).ConfigureAwait(false);
        using (response)
        {
            ThrowIfNotOk(response, "browser-reload");
        }
    }

    private static void ThrowIfNotOk(JsonDocument response, string what)
    {
        if (response.RootElement.TryGetProperty("ok", out var ok) && ok.ValueKind == JsonValueKind.True)
        {
            return;
        }

        throw new InvalidOperationException(
            $"{what} failed: {CoreProtocolParser.TryParseErrorMessage(response)}");
    }

    // Announce a WinUI capture-card shared-memory buffer to the core so it ingests
    // the real BGRA frames as a "capture:<deviceId>" source. Best-effort.
    public async Task RegisterCaptureShmAsync(
        string deviceId,
        string shmName,
        int width,
        int height,
        CancellationToken cancellationToken = default)
    {
        var response = await SendAsync(
            new Dictionary<string, object?>
            {
                ["id"] = NextId(),
                ["type"] = "register-capture-shm",
                ["payload"] = new Dictionary<string, object?>
                {
                    ["deviceId"] = deviceId,
                    ["shmName"] = shmName,
                    ["width"] = width,
                    ["height"] = height
                }
            },
            cancellationToken).ConfigureAwait(false);
        response.Dispose();
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
        // A profile identifies one native-core generation. Keeping the old value
        // makes HandshakeAsync return before the replacement process is ready and
        // lets recovery traffic race its bootstrap handshake.
        _profile = null;

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
            StandardInputEncoding = ChildProcessEncoding,
            StandardOutputEncoding = ChildProcessEncoding,
            StandardErrorEncoding = ChildProcessEncoding,
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

        // Bounded, drop-oldest queue so a slow/busy UI handler never throttles the
        // stdout reader (and thus command-response latency).
        var frameDispatch = Channel.CreateBounded<Action>(new BoundedChannelOptions(8)
        {
            FullMode = BoundedChannelFullMode.DropOldest,
            SingleReader = true,
            SingleWriter = true
        });
        _frameDispatch = frameDispatch;
        _ = Task.Run(async () =>
        {
            await foreach (var action in frameDispatch.Reader.ReadAllAsync().ConfigureAwait(false))
            {
                try { action(); } catch { /* a frame handler must never kill the pump */ }
            }
        });

        _ = Task.Run(() => ReadStdoutLoopAsync(process));
        _process.ErrorDataReceived += (_, e) =>
        {
            if (!string.IsNullOrWhiteSpace(e.Data))
            {
                Debug.WriteLine($"[media-core] {e.Data}");
                WriteCoreLog(e.Data);
            }
        };
        _process.BeginErrorReadLine();
        WriteCoreLog($"[bridge] media core process started (pid {(_process.HasExited ? -1 : _process.Id)})");
    }

    private void DispatchFrame(Action action)
    {
        // Non-blocking: if the consumer is behind, the bounded channel drops the
        // oldest frame rather than stalling the stdout reader.
        _frameDispatch?.Writer.TryWrite(action);
    }

    private static string? _coreLogPath;

    private static void WriteCoreLog(string line)
    {
        try
        {
            _coreLogPath ??= System.IO.Path.Combine(
                System.Environment.GetFolderPath(System.Environment.SpecialFolder.LocalApplicationData),
                "CoreVideoPro",
                "media-core.log");
            var stamped = $"[{System.DateTimeOffset.Now:O}] {line}{System.Environment.NewLine}";
            BoundedLogFile.Append(_coreLogPath, stamped);
        }
        catch
        {
            // Best-effort diagnostic logging; never let it disrupt the media-core pipe.
        }
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
                var frame = frameEvent.Frame;
                // DIAGNOSTIC: measure Zoom transport latency every 30th frame.
                // recv = core-emit -> WinUI off-stdout (base64+pump+stdout);
                // consume = + the C# frame-dispatch queue until the UI handler runs.
                // (This excludes upstream SDK/engine->core, isolating OUR transport.)
                if ((++_zoomFrameCounter % 30) == 0)
                {
                    try
                    {
                        using var doc = System.Text.Json.JsonDocument.Parse(line);
                        if (doc.RootElement.TryGetProperty("frame", out var fe) &&
                            fe.TryGetProperty("emitWallMs", out var ew))
                        {
                            var emit = ew.GetDouble();
                            var recvAge = DateTimeOffset.UtcNow.ToUnixTimeMilliseconds() - emit;
                            PerfLog($"zoom transport emit->recv={recvAge:F0}ms");
                            DispatchFrame(() =>
                            {
                                var consumeAge = DateTimeOffset.UtcNow.ToUnixTimeMilliseconds() - emit;
                                PerfLog($"zoom transport emit->UIhandler={consumeAge:F0}ms (queue={consumeAge - recvAge:F0}ms)");
                                ZoomVideoFrameReceived?.Invoke(frame);
                            });
                            continue;
                        }
                    }
                    catch { }
                }
                DispatchFrame(() => ZoomVideoFrameReceived?.Invoke(frame));
                continue;
            }

            var previewEvent = CoreProtocolParser.TryParseProgramFramePreviewEvent(line);
            if (previewEvent is not null)
            {
                var preview = previewEvent.Preview;
                DispatchFrame(() => ProgramFramePreviewReceived?.Invoke(preview));
                continue;
            }

            var sharedTextureEvent = CoreProtocolParser.TryParseProgramSharedTextureEvent(line);
            if (sharedTextureEvent is not null)
            {
                // The shared-texture handle drives the GPU program display. It is
                // low-rate and the handler is light (update handle + notify), so
                // invoke it directly rather than through the bounded drop-oldest
                // frame queue, where it could be discarded behind high-rate frames
                // and the program would never present the GPU surface.
                var texture = sharedTextureEvent.Texture;
                try { ProgramSharedTextureReceived?.Invoke(texture); } catch { }
                continue;
            }

            var previewTextureEvent = CoreProtocolParser.TryParsePreviewSharedTextureEvent(line);
            if (previewTextureEvent is not null)
            {
                // The core-composited PREVIEW shared-texture handle — low-rate (emitted
                // on structural change only) and the handler is light (update handle +
                // notify), so invoke directly like the program texture.
                var previewTexture = previewTextureEvent.Texture;
                try { PreviewSharedTextureReceived?.Invoke(previewTexture); } catch { }
                continue;
            }

            var participantTextureEvent = CoreProtocolParser.TryParseParticipantSharedTextureEvent(line);
            if (participantTextureEvent is not null)
            {
                // Per-participant GPU texture handle for the multiview tiles — same
                // low-rate, light handler as the program texture; invoke directly.
                var participantTexture = participantTextureEvent.Texture;
                try { ParticipantSharedTextureReceived?.Invoke(participantTexture); } catch { }
                continue;
            }

            var multiviewTextureEvent = CoreProtocolParser.TryParseMultiviewSharedTextureEvent(line);
            if (multiviewTextureEvent is not null)
            {
                // The single core-composited multiview shared-texture handle — low-rate
                // (emitted on structural change only) and the handler is light (update
                // handle + tile rects + notify), so invoke directly like the program texture.
                var multiviewTexture = multiviewTextureEvent.Multiview;
                try { MultiviewSharedTextureReceived?.Invoke(multiviewTexture); } catch { }
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
            RecordCrashEvent(sender as Process, _restarts);
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

        _ = RecoverChildAsync();
    }

    private async Task RecoverChildAsync()
    {
        try
        {
            await EnsureHandshakeProfileAsync(CancellationToken.None).ConfigureAwait(false);

            Dictionary<string, object?>? joinPayload;
            bool rawCapturePaused;
            lock (_gate)
            {
                joinPayload = _zoomJoinRecoveryPayload is null
                    ? null
                    : new Dictionary<string, object?>(_zoomJoinRecoveryPayload, StringComparer.Ordinal);
                rawCapturePaused = _zoomRawCapturePaused;
            }

            if (joinPayload is null)
            {
                StatusChanged?.Invoke("Media core recovered");
                return;
            }

            var maxAttempts = Math.Max(1, _options.ZoomRecoveryMaxAttempts);
            RawCaptureSnapshot? snapshot = null;
            for (var attempt = 1; attempt <= maxAttempts; attempt++)
            {
                if (attempt > 1)
                {
                    var delayMs = Math.Max(0, _options.ZoomRecoveryRetryDelayMs);
                    StatusChanged?.Invoke($"Media core recovered — Zoom rejoin retry {attempt}/{maxAttempts}…");
                    WriteCoreLog($"[bridge] Zoom recovery retry {attempt}/{maxAttempts} after {delayMs}ms SDK teardown cooldown");
                    await Task.Delay(delayMs, CancellationToken.None).ConfigureAwait(false);
                }
                else
                {
                    StatusChanged?.Invoke("Media core recovered — rejoining Zoom…");
                }

                var response = await SendAsync(
                    new Dictionary<string, object?>
                    {
                        ["id"] = NextId(),
                        ["type"] = "zoom-join",
                        ["payload"] = joinPayload
                    },
                    CancellationToken.None,
                    _options.ZoomJoinRequestTimeoutMs).ConfigureAwait(false);

                using (response)
                {
                    snapshot = CoreProtocolParser.TryParseCaptureSnapshot(response, "zoom-join")
                        ?? throw new InvalidOperationException(
                            CoreProtocolParser.DescribeUnexpectedCaptureResponse(response, "zoom-join"));
                }

                if (ZoomMediaSpineSnapshotMerger.NormalizeMeetingState(snapshot.MeetingState) == "in_meeting")
                {
                    break;
                }

                WriteCoreLog($"[bridge] Zoom recovery attempt {attempt}/{maxAttempts} returned meeting state '{snapshot.MeetingState}'");
            }

            if (snapshot is null || ZoomMediaSpineSnapshotMerger.NormalizeMeetingState(snapshot.MeetingState) != "in_meeting")
            {
                throw new InvalidOperationException($"Zoom recovery returned meeting state '{snapshot?.MeetingState ?? "unknown"}' after {maxAttempts} attempts.");
            }

            if (rawCapturePaused)
            {
                var stopResponse = await SendAsync(
                    new Dictionary<string, object?>
                    {
                        ["id"] = NextId(),
                        ["type"] = "zoom-stop-capture"
                    },
                    CancellationToken.None).ConfigureAwait(false);
                using (stopResponse)
                {
                    snapshot = CoreProtocolParser.TryParseCaptureSnapshot(stopResponse, "zoom-stop-capture")
                        ?? snapshot;
                }
            }

            ZoomRecovered?.Invoke(snapshot);
            StatusChanged?.Invoke(rawCapturePaused
                ? "Media core and Zoom recovered — capture remains paused"
                : "Media core and Zoom recovered");
        }
        catch (Exception ex)
        {
            WriteCoreLog($"[bridge] recovery failed: {ex.GetType().Name}: {ex.Message}");
            StatusChanged?.Invoke($"Media core recovered, but Zoom rejoin failed: {ex.Message}");
        }
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
        var requestType = payload.TryGetValue("type", out var typeValue) ? typeValue as string : null;
        await using var registration = timeoutCts.Token.Register(() =>
        {
            lock (_gate)
            {
                _pending.Remove(id);
            }

            WriteCoreLog($"[bridge] TIMEOUT id={id} type={requestType} after {timeoutMs ?? _options.RequestTimeoutMs}ms");
            tcs.TrySetException(new TimeoutException($"media core request {id} ({requestType}) timed out."));
        });

        var json = JsonSerializer.Serialize(payload, MediaCoreJson.Options);
        // The timeout must cover gate-acquire + write + flush, not just the response
        // wait. If the core stalls reading stdin (e.g. during a long blocking join),
        // the OS pipe buffer fills and the write would otherwise block forever.
        await _stdinGate.WaitAsync(timeoutCts.Token).ConfigureAwait(false);
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

                if (requestType == "zoom-join")
                {
                    WriteCoreLog($"[bridge] -> id={id} type=zoom-join bytes={json.Length}");
                }

                await stdin.WriteLineAsync(json.AsMemory(), timeoutCts.Token).ConfigureAwait(false);
                await stdin.FlushAsync(timeoutCts.Token).ConfigureAwait(false);

                if (requestType == "zoom-join")
                {
                    WriteCoreLog($"[bridge] sent id={id} type=zoom-join (awaiting core response)");
                }
            }
            catch (Exception ex) when (ex is InvalidOperationException or IOException or ObjectDisposedException or OperationCanceledException)
            {
                lock (_gate)
                {
                    _pending.Remove(id);
                }

                if (requestType == "zoom-join")
                {
                    WriteCoreLog($"[bridge] write FAILED id={id} type=zoom-join: {ex.GetType().Name}: {ex.Message}");
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

    /// <summary>
    /// Records a child exit into the bounded crash buffer. Caller must hold <see cref="_gate"/>.
    /// </summary>
    private void RecordCrashEvent(Process? exited, int restartCount)
    {
        int? exitCode = null;
        try
        {
            if (exited is { HasExited: true })
            {
                exitCode = exited.ExitCode;
            }
        }
        catch
        {
            // Exit code may be unavailable; record as null.
        }

        _crashEvents.AddLast(new MediaCoreCrashEvent
        {
            At = DateTime.UtcNow.ToString("o", System.Globalization.CultureInfo.InvariantCulture),
            ExitCode = exitCode,
            RestartCount = restartCount
        });

        while (_crashEvents.Count > MaxCrashEvents)
        {
            _crashEvents.RemoveFirst();
        }
    }

    private void RaiseHealth() => HealthChanged?.Invoke(Health);

    public ValueTask DisposeAsync()
    {
        Stop();
        return ValueTask.CompletedTask;
    }
}
