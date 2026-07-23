using System.Diagnostics;
using CoreVideoPro.MediaCore.Models;
using CoreVideoPro.MediaCore.Services;

namespace CoreVideoPro.WinUI.Services;

/// <summary>
/// Orchestrates opt-in telemetry (spec §S3): a daily heartbeat while running and
/// one session-end event at app shutdown. NOTHING is sent unless the operator
/// has explicitly toggled consent ON (<see cref="TelemetryConsentStore"/>) AND
/// the ingest endpoint/key are configured (reuses S1's
/// <see cref="CrashReportingConfig"/>). Every send logs the exact payload to
/// launch.log first, so egress is always auditable. Failures are silent-safe:
/// they never block shutdown and never nag (spec §7).
///
/// Data is sourced ONLY from the media-core snapshot accessor and a portable
/// hardware probe — never from StudioViewModel transport state — so it does not
/// contend with the concurrent transport refactor.
/// </summary>
public sealed class TelemetryEventService : IDisposable
{
    private static readonly TimeSpan HeartbeatInterval = TimeSpan.FromHours(24);
    private static readonly TimeSpan HeartbeatSendTimeout = TimeSpan.FromSeconds(15);
    // Shutdown must stay prompt (MainWindow force-exits at ~6 s): a tight cap.
    private static readonly TimeSpan SessionEndSendTimeout = TimeSpan.FromSeconds(2);

    private readonly Func<NativeMediaCoreStateSnapshot?> _snapshotAccessor;
    private readonly TelemetryConsentStore _consentStore;
    private readonly CrashReportingConfig _config;
    private readonly Func<CrashReportWatermark?> _crashWatermarkAccessor;
    private readonly Func<string> _versionAccessor;
    private readonly Func<string?> _gpuTierProbe;
    private readonly Func<Uri, string, string, CancellationToken, Task<TelemetrySendResult>> _transport;

    private readonly Stopwatch _sessionClock = new();
    private readonly object _gate = new();
    private Timer? _heartbeatTimer;
    private string? _gpuTier;
    private bool _gpuProbed;
    private bool _sessionEndSent;
    private bool _disposed;

    public TelemetryEventService(
        Func<NativeMediaCoreStateSnapshot?> snapshotAccessor,
        TelemetryConsentStore? consentStore = null,
        CrashReportingConfig? config = null,
        Func<CrashReportWatermark?>? crashWatermarkAccessor = null,
        Func<string>? versionAccessor = null,
        Func<string?>? gpuTierProbe = null,
        Func<Uri, string, string, CancellationToken, Task<TelemetrySendResult>>? transport = null)
    {
        _snapshotAccessor = snapshotAccessor ?? throw new ArgumentNullException(nameof(snapshotAccessor));
        _consentStore = consentStore ?? new TelemetryConsentStore(TelemetryConsentStore.DefaultPath());
        _config = config ?? CrashReportingConfig.FromEnvironment();
        _crashWatermarkAccessor = crashWatermarkAccessor ?? DefaultCrashWatermark;
        _versionAccessor = versionAccessor ?? UpdateNotificationService.ResolveCurrentVersion;
        _gpuTierProbe = gpuTierProbe ?? GpuTierProbe.TryProbe;
        _transport = transport ?? DefaultTransportAsync;
    }

    /// <summary>
    /// Default HTTP transport: a fresh client per send. The caller's CTS already
    /// bounds each send (tight at shutdown, generous for the heartbeat), so the
    /// client just needs a backstop timeout.
    /// </summary>
    private static async Task<TelemetrySendResult> DefaultTransportAsync(
        Uri endpoint, string apiKey, string wireJson, CancellationToken cancellationToken)
    {
        using var client = new TelemetryEventClient();
        return await client.SendAsync(endpoint, apiKey, wireJson, cancellationToken).ConfigureAwait(false);
    }

    /// <summary>Endpoint + key present (independent of consent).</summary>
    public bool IsConfigured => _config.Enabled;

    /// <summary>
    /// Begin the session clock and (if configured) the daily heartbeat. Safe to
    /// call once at startup. The heartbeat only actually SENDS when consent is
    /// ON at fire time, so starting the timer regardless is harmless.
    /// </summary>
    public void Start()
    {
        lock (_gate)
        {
            if (_disposed || _sessionClock.IsRunning)
            {
                return;
            }

            _sessionClock.Start();
            // Warm the GPU band off the UI thread so the first preview/send has it.
            _ = Task.Run(() => EnsureGpuTier());

            if (_config.Enabled)
            {
                _heartbeatTimer = new Timer(_ => OnHeartbeat(), null, HeartbeatInterval, HeartbeatInterval);
            }
        }
    }

    public long SessionLengthSeconds => (long)_sessionClock.Elapsed.TotalSeconds;

    /// <summary>
    /// Builds the exact payload that WOULD be sent — used by the settings
    /// "preview what's sent" affordance. Works regardless of consent so the
    /// operator can inspect the egress BEFORE opting in.
    /// </summary>
    public TelemetryEventPayload BuildPayload(string eventName)
    {
        var machine = MachineClassProbe.Probe(EnsureGpuTier());
        var crashCount = TelemetryCrashCount.CountSince(
            SafeLoadWatermark(), _consentStore.Load().LastSentUtc);

        return TelemetryPayloadBuilder.Build(
            eventName,
            SafeVersion(),
            SessionLengthSeconds,
            _snapshotAccessor(),
            crashCount,
            machine);
    }

    /// <summary>Indented JSON preview of the session-end payload for the settings UI.</summary>
    public string BuildPreviewJson() =>
        TelemetryPayloadBuilder.SerializePreview(BuildPayload(TelemetryEventNames.SessionEnd));

    /// <summary>
    /// Fire-and-forget session-end send for the app-shutdown path. Gated on
    /// consent + config; bounded by a tight timeout so the app closes promptly
    /// regardless of the network; NEVER honors Retry-After (spec §S3.4). Returns
    /// immediately — callers must not await it on the shutdown critical path.
    /// </summary>
    public void FireSessionEnd()
    {
        if (!ConsentAndConfigReady())
        {
            return;
        }

        lock (_gate)
        {
            if (_sessionEndSent)
            {
                return;
            }

            _sessionEndSent = true;
        }

        _ = Task.Run(async () =>
        {
            try
            {
                using var cts = new CancellationTokenSource(SessionEndSendTimeout);
                // Session-end NEVER honors Retry-After (spec §S3.4): the caller
                // ignores the result and the app closes regardless.
                await SendAsync(TelemetryEventNames.SessionEnd, cts.Token).ConfigureAwait(false);
            }
            catch (Exception ex)
            {
                SafeLog($"telemetry: session-end send failed silently ({ex.GetType().Name}: {ex.Message})");
            }
        });
    }

    private void OnHeartbeat()
    {
        if (!ConsentAndConfigReady())
        {
            return;
        }

        _ = Task.Run(async () =>
        {
            try
            {
                using var cts = new CancellationTokenSource(HeartbeatSendTimeout);
                var result = await SendAsync(TelemetryEventNames.Heartbeat, cts.Token).ConfigureAwait(false);

                // Honor Retry-After for the heartbeat only: push the next fire out.
                if (result is { Status: TelemetrySendStatus.RetryLater, RetryAfter: { } delay } && delay > TimeSpan.Zero)
                {
                    lock (_gate)
                    {
                        _heartbeatTimer?.Change(delay, HeartbeatInterval);
                    }
                }
            }
            catch (Exception ex)
            {
                SafeLog($"telemetry: heartbeat send failed silently ({ex.GetType().Name}: {ex.Message})");
            }
        });
    }

    private async Task<TelemetrySendResult> SendAsync(
        string eventName, CancellationToken cancellationToken)
    {
        var payload = BuildPayload(eventName);
        var wire = TelemetryPayloadBuilder.SerializeWire(payload);

        // Audit trail (spec §7 — inspectable before egress): the exact JSON is
        // logged locally before the POST.
        SafeLog($"telemetry: sending '{eventName}' ({wire.Length} bytes): {wire}");

        var result = await _transport(
            new Uri(_config.Endpoint!, UriKind.Absolute), _config.ApiKey!, wire, cancellationToken)
            .ConfigureAwait(false);

        if (result.Status == TelemetrySendStatus.Accepted)
        {
            SafeMarkSent();
            SafeLog($"telemetry: '{eventName}' accepted (reportId={result.ReportId ?? "none"})");
        }
        else
        {
            SafeLog($"telemetry: '{eventName}' not accepted ({result.Status}, http={result.HttpStatus?.ToString() ?? "n/a"}): {result.Message}");
        }

        return result;
    }

    private bool ConsentAndConfigReady()
    {
        if (!_config.Enabled)
        {
            return false;
        }

        try
        {
            return _consentStore.Load().Enabled;
        }
        catch
        {
            return false; // never send on a bad consent read
        }
    }

    private string? EnsureGpuTier()
    {
        lock (_gate)
        {
            if (_gpuProbed)
            {
                return _gpuTier;
            }
        }

        string? tier = null;
        try
        {
            tier = _gpuTierProbe();
        }
        catch
        {
            tier = null;
        }

        lock (_gate)
        {
            _gpuTier = tier;
            _gpuProbed = true;
            return _gpuTier;
        }
    }

    private CrashReportWatermark? SafeLoadWatermark()
    {
        try
        {
            return _crashWatermarkAccessor();
        }
        catch
        {
            return null;
        }
    }

    private string SafeVersion()
    {
        try
        {
            var v = _versionAccessor();
            return string.IsNullOrWhiteSpace(v) ? "0.0.0" : v;
        }
        catch
        {
            return "0.0.0";
        }
    }

    private void SafeMarkSent()
    {
        try
        {
            _consentStore.MarkSent(DateTimeOffset.UtcNow);
        }
        catch
        {
            // A failed watermark write only means the next crash-count delta is
            // slightly generous — never fatal.
        }
    }

    private static CrashReportWatermark? DefaultCrashWatermark()
    {
        try
        {
            return new CrashReportWatermarkStore(CrashReportWatermarkStore.DefaultPath()).Load();
        }
        catch
        {
            return null;
        }
    }

    private static void SafeLog(string message)
    {
        try
        {
            LaunchLog.Write(message);
        }
        catch
        {
            // logging must never throw into the send path
        }
    }

    public void Dispose()
    {
        lock (_gate)
        {
            _disposed = true;
            _heartbeatTimer?.Dispose();
            _heartbeatTimer = null;
        }
    }
}
