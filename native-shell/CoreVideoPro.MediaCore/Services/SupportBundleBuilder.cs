using System.Globalization;
using System.Text;
using System.Text.Encodings.Web;
using System.Text.Json;
using System.Text.Json.Serialization;
using System.Text.RegularExpressions;
using CoreVideoPro.MediaCore.Models;

namespace CoreVideoPro.MediaCore.Services;

/// <summary>
/// App/runtime metadata captured alongside a support bundle. Mirrors the React
/// shell's SupportBundleContext app/runtime fields (src/engine/contracts.ts,
/// src/engine/supportBundle.ts).
/// </summary>
public sealed class SupportBundleAppInfo
{
    public string Name { get; init; } = "CoreVideo Pro";
    public string Version { get; init; } = "0.1.0";
    public string Platform { get; init; } = "winui-desktop";
    public string Host { get; init; } = Environment.MachineName;
    public string RuntimeLabel { get; init; } = "Native media core";
}

/// <summary>
/// A redactable output destination descriptor supplied by the shell. Endpoint and
/// stream key are redacted before they reach the bundle (mirrors the TS bundle's
/// output.destinations redaction).
/// </summary>
public sealed class SupportBundleOutputDestination
{
    public required string Id { get; init; }
    public string Name { get; init; } = string.Empty;
    public string Protocol { get; init; } = "rtmp";
    public bool Enabled { get; init; }
    public bool Active { get; init; }
    public string? Resolution { get; init; }
    public string? Fps { get; init; }
    public string? Codec { get; init; }
    public string? EncoderMode { get; init; }
    public double? TargetBitrateMbps { get; init; }
    public string? Format { get; init; }
    public string? Quality { get; init; }
    public string? Endpoint { get; init; }
    public string? StreamKey { get; init; }
}

/// <summary>
/// Builds a REDACTED support bundle (diagnostics + crash/recovery state) and
/// serializes it to disk. C# port of src/engine/supportBundle.ts — gathers the
/// latest media-core snapshot, supervisor health + crash events, and app/runtime
/// metadata, then redacts stream keys / endpoints / URLs before writing JSON.
/// </summary>
public static class SupportBundleBuilder
{
    private static readonly JsonSerializerOptions SerializerOptions = new()
    {
        PropertyNamingPolicy = JsonNamingPolicy.CamelCase,
        DefaultIgnoreCondition = JsonIgnoreCondition.WhenWritingNull,
        WriteIndented = true,
        // Keep characters like '/' and '&' readable in the on-disk bundle.
        Encoder = JavaScriptEncoder.UnsafeRelaxedJsonEscaping
    };

    private static readonly Regex SecretQueryParam = new(
        "(token|key|passphrase|password|secret|code)=([^&\\s]+)",
        RegexOptions.IgnoreCase | RegexOptions.Compiled);

    /// <summary>
    /// Assembles a redacted <see cref="SupportBundle"/> in memory.
    /// </summary>
    public static SupportBundle Build(
        NativeMediaCoreStateSnapshot? snapshot,
        MediaCoreHealth health,
        SupportBundleAppInfo? app = null,
        IReadOnlyList<SupportBundleOutputDestination>? outputs = null,
        DateTimeOffset? createdAt = null)
    {
        ArgumentNullException.ThrowIfNull(health);

        app ??= new SupportBundleAppInfo();
        var stamp = createdAt ?? DateTimeOffset.UtcNow;
        var createdAtIso = stamp.UtcDateTime.ToString("o", CultureInfo.InvariantCulture);

        var runtime = new SupportBundleRuntime
        {
            Status = health.Stopped ? "stopped" : health.Recovering ? "recovering" : "running",
            Label = app.RuntimeLabel,
            Host = app.Host,
            Platform = app.Platform,
            RestartCount = health.RestartCount,
            Recovering = health.Recovering,
            Warnings = snapshot?.Warnings.ToArray() ?? []
        };

        var crashEvents = health.CrashEvents
            .Select(crash => new SupportBundleCrashEvent
            {
                At = crash.At,
                ExitCode = crash.ExitCode,
                RestartCount = crash.RestartCount
            })
            .ToArray();

        var mediaCore = snapshot is not null ? SummarizeMediaCore(snapshot) : null;
        var redactedOutputs = (outputs ?? [])
            .Select(destination => new SupportBundleOutputDestinationSummary
            {
                Id = destination.Id,
                Name = destination.Name,
                Protocol = destination.Protocol,
                Enabled = destination.Enabled,
                Active = destination.Active,
                Resolution = destination.Resolution,
                Fps = destination.Fps,
                Codec = destination.Codec,
                EncoderMode = destination.EncoderMode,
                TargetBitrateMbps = destination.TargetBitrateMbps,
                Format = destination.Format,
                Quality = destination.Quality,
                Endpoint = RedactEndpoint(destination.Endpoint),
                StreamKey = RedactSecret(destination.StreamKey)
            })
            .ToArray();

        var triageLines = BuildTriageLines(snapshot, runtime, crashEvents, mediaCore);

        return new SupportBundle
        {
            Id = $"support-{stamp.UtcDateTime:yyyyMMddHHmmss}",
            CreatedAt = createdAtIso,
            App = new SupportBundleAppSummary
            {
                Name = app.Name,
                Version = app.Version,
                Platform = app.Platform
            },
            SummaryText = string.Join("\n", triageLines),
            TriageLines = triageLines,
            Outputs = redactedOutputs,
            MediaCore = mediaCore,
            Runtime = runtime,
            CrashRecovery = crashEvents.Length > 0
                ? new SupportBundleCrashRecovery { Events = crashEvents }
                : null,
            Warnings = mediaCore?.Warnings ?? snapshot?.Warnings.ToArray() ?? []
        };
    }

    /// <summary>
    /// Builds the bundle and writes it as redacted JSON to <paramref name="filePath"/>.
    /// </summary>
    public static async Task<SupportBundle> WriteAsync(
        string filePath,
        NativeMediaCoreStateSnapshot? snapshot,
        MediaCoreHealth health,
        SupportBundleAppInfo? app = null,
        IReadOnlyList<SupportBundleOutputDestination>? outputs = null,
        DateTimeOffset? createdAt = null,
        CancellationToken cancellationToken = default)
    {
        var bundle = Build(snapshot, health, app, outputs, createdAt);
        var directory = Path.GetDirectoryName(filePath);
        if (!string.IsNullOrEmpty(directory))
        {
            Directory.CreateDirectory(directory);
        }

        var json = Serialize(bundle);
        await File.WriteAllTextAsync(filePath, json, new UTF8Encoding(false), cancellationToken)
            .ConfigureAwait(false);
        return bundle;
    }

    public static string Serialize(SupportBundle bundle) =>
        JsonSerializer.Serialize(bundle, SerializerOptions);

    /// <summary>
    /// Default on-disk location for an exported support bundle.
    /// </summary>
    public static string DefaultBundlePath(DateTimeOffset? createdAt = null)
    {
        var stamp = (createdAt ?? DateTimeOffset.UtcNow).UtcDateTime;
        var folder = Path.Combine(
            Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData),
            "CoreVideoPro",
            "support-bundles");
        return Path.Combine(folder, $"support-{stamp:yyyyMMddHHmmss}.json");
    }

    private static SupportBundleMediaCore SummarizeMediaCore(NativeMediaCoreStateSnapshot snapshot)
    {
        var source = snapshot.SourceSnapshot;
        var compositor = snapshot.Compositor;
        var transport = snapshot.ProgramTransport;
        var encoder = snapshot.EncoderSession;
        var senders = snapshot.OutputSenderSession;
        var audio = snapshot.AudioMixSession;
        var routing = snapshot.AudioRoutingMatrix;
        var capture = snapshot.CaptureAudioSources;

        return new SupportBundleMediaCore
        {
            SceneId = snapshot.SceneId,
            RenderPlanId = snapshot.RenderPlan.RenderPlanId,
            Source = new SupportBundleMediaCoreSource
            {
                AdapterId = source.AdapterId,
                Kind = source.Kind,
                Status = source.Status,
                SubscribedSourceCount = source.SubscribedSourceCount,
                DeliveredFrameCount = snapshot.FrameCount,
                LiveFrameCount = source.LiveFrameCount,
                StaleFrameCount = source.StaleFrameCount,
                Backing = SourceBacking(source),
                DroppedFrameCount = source.DroppedFrameCount,
                LowResolutionFrameCount = source.LowResolutionFrameCount
            },
            Compositor = new SupportBundleMediaCoreCompositor
            {
                Status = compositor.Status,
                ProgramFrameCount = compositor.ProgramFrameCount,
                DroppedFrameCount = compositor.DroppedFrameCount,
                DegradedFrameCount = compositor.DegradedFrameCount
            },
            Transport = new SupportBundleMediaCoreTransport
            {
                Status = transport.Status,
                FrameNumber = transport.FrameNumber,
                LatencyMs = transport.LatencyMs
            },
            Encoder = new SupportBundleMediaCoreEncoder
            {
                Status = encoder.Status,
                Lifecycle = encoder.Lifecycle.Status,
                TargetCount = encoder.Targets.Count
            },
            Audio = SummarizeAudio(audio, routing, capture, senders, snapshot.Recording),
            Senders = new SupportBundleMediaCoreSenders
            {
                Status = senders.Status,
                ActiveSenderCount = senders.ActiveSenderCount,
                Destinations = senders.Senders
                    .Select(sender => new SupportBundleMediaCoreSender
                    {
                        // Destination strings can contain endpoints/URLs; redact.
                        Destination = RedactEndpoint(sender.Destination),
                        Status = sender.Status,
                        StartedAtMs = sender.StartedAtMs,
                        StoppedAtMs = sender.StoppedAtMs,
                        LastFrameNumber = sender.LastFrameNumber,
                        FramesSent = sender.FramesSent,
                        RetryCount = sender.RetryCount,
                        LatencyMs = sender.LatencyMs,
                        BitrateMbps = sender.BitrateMbps,
                        AudioFramesSent = sender.AudioFramesSent,
                        AudioBytesSent = sender.AudioBytesSent,
                        AudioChannels = sender.AudioChannels,
                        AudioSampleRate = sender.AudioSampleRate,
                        Warning = sender.Warning
                    })
                    .ToArray()
            },
            Recording = snapshot.Recording is { } recording
                ? new SupportBundleMediaCoreRecording
                {
                    Status = recording.Status,
                    WriterStatus = recording.WriterStatus,
                    TotalFramesWritten = recording.TotalFramesWritten,
                    TotalDroppedFrames = recording.TotalDroppedFrames,
                    EstimatedDiskRateMBps = recording.EstimatedDiskRateMBps,
                    Proof = recording.Proof is { } proof
                        ? new SupportBundleMediaCoreRecordingProof
                        {
                            AudioPacketsObserved = proof.AudioPacketsObserved,
                            AudioPresent = proof.AudioPresent,
                            AudioSampleCount = proof.AudioSampleCount,
                            AudioChannels = proof.AudioChannels,
                            AudioSampleRate = proof.AudioSampleRate,
                            MetadataValid = proof.MetadataValid,
                            ContainerFormat = proof.ContainerFormat,
                            AudioCodec = proof.AudioCodec,
                            AudioBitrateKbps = proof.AudioBitrateKbps,
                            TargetBitrateMbps = proof.TargetBitrateMbps
                        }
                        : null,
                    Streams = recording.Streams
                        .Select(stream => new SupportBundleMediaCoreRecordingStream
                        {
                            Kind = stream.Kind,
                            ParticipantId = stream.ParticipantId,
                            Status = stream.Status,
                            FramesWritten = stream.FramesWritten,
                            DroppedFrames = stream.DroppedFrames,
                            BytesWritten = stream.BytesWritten,
                            Warning = stream.Warning
                        })
                        .ToArray()
                }
                : null,
            OperatorActions = snapshot.OperatorActions
                .Select(action => new SupportBundleOperatorAction
                {
                    ActionId = action.ActionId,
                    Severity = action.Severity,
                    Area = action.Area,
                    Title = action.Title,
                    Detail = action.Detail,
                    Command = action.Command,
                    RelatedId = action.RelatedId
                })
                .ToArray(),
            EventLog = snapshot.EventLog
                .Select(entry => new SupportBundleEvent
                {
                    EventId = entry.EventId,
                    AtMs = entry.AtMs,
                    Severity = entry.Severity,
                    Area = entry.Area,
                    Title = entry.Title,
                    Detail = entry.Detail,
                    RelatedId = entry.RelatedId,
                    CommandType = entry.CommandType
                })
                .ToArray(),
            Warnings = snapshot.Warnings.ToArray()
        };
    }

    private static SupportBundleMediaCoreAudio SummarizeAudio(
        NativeMediaCoreAudioMixSession session,
        NativeMediaCoreAudioRoutingMatrix routing,
        NativeMediaCoreCaptureAudioSources capture,
        NativeMediaCoreOutputSenderSession? senders,
        NativeMediaCoreRecordingSession? recording)
    {
        var warnings = session.Warnings.ToArray();
        var participants = session.Participants;
        var peakOutput = participants.Count > 0 ? participants.Max(p => p.OutputLevel) : 0;
        var validationChecklist = AudioValidationChecklistBuilder.Build(session, capture, senders, recording);

        return new SupportBundleMediaCoreAudio
        {
            Status = session.Status,
            MasterLevel = session.MasterLevel,
            LoudnessLufs = session.LoudnessLufs,
            LimiterActive = session.LimiterActive,
            MixedFrameCount = session.MixedFrameCount,
            MonitorEnabled = session.MonitorEnabled,
            MonitorStatus = session.MonitorStatus,
            MonitorDeviceName = session.MonitorDeviceName,
            MonitorFramesPlayed = session.MonitorFramesPlayed,
            ParticipantCount = participants.Count,
            MutedParticipantCount = participants.Count(p => p.Muted),
            BoostedParticipantCount = participants.Count(p => p.Status == "boosting"),
            DuckedParticipantCount = participants.Count(p => p.Status == "ducking"),
            LimitedParticipantCount = participants.Count(p => p.LimiterActive),
            LowLevelParticipantCount = participants.Count(p => p.NoiseSuppression && p.InputLevel < 35),
            PeakOutputLevel = peakOutput,
            WarningCount = warnings.Length,
            WarningCategories = CategorizeAudioWarnings(session),
            Warnings = warnings,
            ValidationChecklist = validationChecklist,
            ValidationSummary = AudioValidationChecklistBuilder.SummarizeAcceptance(validationChecklist),
            ValidationFirstIssue = AudioValidationChecklistBuilder.FindFirstBlockingStage(validationChecklist),
            FullChainValidationSummary = AudioValidationChecklistBuilder.SummarizeFullChain(validationChecklist),
            Routing = new SupportBundleMediaCoreAudioRouting
            {
                Status = routing.Status,
                RoutedSendCount = routing.RoutedSendCount,
                RoutedSourceCount = routing.RoutedSourceCount,
                ProgramTapFrames = routing.ProgramTapFrames,
                BusTaps = routing.BusTaps
                    .Select(tap => new SupportBundleMediaCoreAudioBusTap
                    {
                        BusId = tap.BusId,
                        Channels = tap.Channels,
                        Frames = tap.Frames,
                        PeakDbfs = tap.PeakDbfs,
                        RmsDbfs = tap.RmsDbfs
                    })
                    .ToArray(),
                Warnings = routing.Warnings.ToArray()
            },
            Capture = new SupportBundleMediaCoreCaptureAudio
            {
                Status = capture.Status,
                SourceCount = capture.SourceCount,
                PairedCount = capture.PairedCount,
                StreamingCount = capture.StreamingCount,
                CaptureFramesReceived = capture.CaptureFramesReceived,
                RoutedMasterFrames = capture.RoutedMasterFrames,
                RoutedStreamFrames = capture.RoutedStreamFrames,
                RoutedMonitorFrames = capture.RoutedMonitorFrames,
                FallbackMonitorFrames = capture.FallbackMonitorFrames,
                MonitorFramesPlayed = capture.MonitorFramesPlayed,
                Sources = capture.Sources
                    .Select(source => new SupportBundleMediaCoreCaptureAudioSource
                    {
                        CaptureDeviceId = source.CaptureDeviceId,
                        SourceId = source.SourceId,
                        AudioDeviceId = source.AudioDeviceId,
                        AudioDeviceName = source.AudioDeviceName,
                        AudioSourceKind = source.AudioSourceKind,
                        NativeAudioDeviceId = source.NativeAudioDeviceId,
                        AudioDriverName = source.AudioDriverName,
                        Embedded = source.Embedded,
                        AudioSyncOffsetMs = source.AudioSyncOffsetMs,
                        Paired = source.Paired,
                        CaptureStreaming = source.CaptureStreaming,
                        CaptureFramesReceived = source.CaptureFramesReceived,
                        CaptureFramesRendered = source.CaptureFramesRendered,
                        CaptureQueuedFrames = source.CaptureQueuedFrames,
                        CaptureUnderrunCount = source.CaptureUnderrunCount,
                        EmptyPacketPolls = source.EmptyPacketPolls,
                        CaptureStartedAtMs = source.CaptureStartedAtMs,
                        CaptureLastFrameAtMs = source.CaptureLastFrameAtMs,
                        CaptureLastFrameAgeMs = source.CaptureLastFrameAgeMs,
                        CaptureStoppedAtMs = source.CaptureStoppedAtMs,
                        CaptureSampleRate = source.CaptureSampleRate,
                        CaptureChannels = source.CaptureChannels,
                        PeakDbfs = source.PeakDbfs,
                        RmsDbfs = source.RmsDbfs,
                        SignalPresent = source.SignalPresent,
                        EndpointId = source.EndpointId,
                        EndpointName = source.EndpointName,
                        LastError = source.LastError,
                        Warning = source.Warning
                    })
                    .ToArray(),
                Warnings = capture.Warnings.ToArray()
            }
        };
    }

    private static string[] CategorizeAudioWarnings(NativeMediaCoreAudioMixSession session)
    {
        var categories = new List<string>();
        var text = string.Join(" ", session.Warnings).ToLowerInvariant();

        void Add(string category)
        {
            if (!categories.Contains(category))
            {
                categories.Add(category);
            }
        }

        if (text.Contains("underrun") || text.Contains("timing gap"))
        {
            Add("underrun");
        }

        if (text.Contains("clipping") || text.Contains("clip"))
        {
            Add("clipping");
        }

        if (text.Contains("a/v") || text.Contains("av sync") || text.Contains("sync offset") || text.Contains("drift"))
        {
            Add("av-sync");
        }

        if (session.LimiterActive || text.Contains("limiter"))
        {
            Add("limiter");
        }

        if (session.Participants.Any(p => p.NoiseSuppression && p.InputLevel < 35))
        {
            Add("low-level");
        }

        if (text.Contains("noise suppression"))
        {
            Add("noise-suppression");
        }

        return categories.ToArray();
    }

    private static string SourceBacking(NativeMediaCoreFrameSourceSnapshot source) =>
        source.Kind == "zoom-sdk" && !source.AdapterId.StartsWith("renderer-", StringComparison.Ordinal)
            ? "native-frame"
            : "fallback-simulated";

    private static string[] BuildTriageLines(
        NativeMediaCoreStateSnapshot? snapshot,
        SupportBundleRuntime runtime,
        IReadOnlyList<SupportBundleCrashEvent> crashEvents,
        SupportBundleMediaCore? mediaCore)
    {
        var lines = new List<string>
        {
            $"Runtime: {runtime.Label} ({runtime.Status})"
        };

        if (runtime.RestartCount > 0)
        {
            lines.Add($"Media core restarts: {runtime.RestartCount}");
        }

        if (snapshot is not null)
        {
            lines.Add($"Program scene: {snapshot.SceneId ?? "none"}; render plan: {snapshot.RenderPlan.RenderPlanId}");
            lines.Add(
                $"Video source: {SourceBacking(snapshot.SourceSnapshot)}; subscribed {snapshot.SourceSnapshot.SubscribedSourceCount}; delivered {snapshot.FrameCount}; stale {snapshot.SourceSnapshot.StaleFrameCount}");
            lines.Add($"Output warnings: {snapshot.Warnings.Count}");

            if (mediaCore is not null)
            {
                var audio = mediaCore.Audio;
                var includeAudioValidation =
                    !string.IsNullOrWhiteSpace(audio.FullChainValidationSummary) &&
                    !audio.FullChainValidationSummary.StartsWith("Full audio chain validated", StringComparison.OrdinalIgnoreCase);
                if (audio.Status == "warning" || audio.WarningCount > 0 || audio.LimiterActive || includeAudioValidation)
                {
                    var categories = audio.WarningCategories.Count > 0
                        ? string.Join(", ", audio.WarningCategories)
                        : "none";
                    lines.Add($"Audio diagnostics: {audio.Status}; categories: {categories}; warnings: {audio.WarningCount}");
                    if (!string.IsNullOrWhiteSpace(audio.ValidationSummary))
                    {
                        lines.Add(audio.ValidationSummary);
                    }

                    if (audio.ValidationChecklist.Count > 0)
                    {
                        lines.Add($"Audio validation checklist: {string.Join(" | ", audio.ValidationChecklist)}");
                    }

                    if (!string.IsNullOrWhiteSpace(audio.ValidationFirstIssue))
                    {
                        lines.Add(audio.ValidationFirstIssue);
                    }

                    if (!string.IsNullOrWhiteSpace(audio.FullChainValidationSummary))
                    {
                        lines.Add(audio.FullChainValidationSummary);
                    }
                }

                if (audio.Capture.SourceCount > 0)
                {
                    lines.Add(
                        $"Audio capture: {audio.Capture.StreamingCount}/{audio.Capture.SourceCount} streaming; PCM {audio.Capture.CaptureFramesReceived}; master {audio.Capture.RoutedMasterFrames}; stream {audio.Capture.RoutedStreamFrames}; MON {audio.Capture.RoutedMonitorFrames}; monitor played {audio.Capture.MonitorFramesPlayed}");

                    if (audio.Capture.CaptureFramesReceived > 0 && audio.Capture.RoutedMasterFrames <= 0)
                    {
                        lines.Add("Audio capture fault: source PCM is present but not reaching the master bus.");
                    }

                    if (audio.Capture.CaptureFramesReceived > 0 && audio.Capture.RoutedStreamFrames <= 0)
                    {
                        lines.Add("Audio stream fault: source PCM is present but not reaching the stream bus.");
                    }

                    if (audio.Capture.RoutedMasterFrames > 0 && audio.Capture.RoutedMonitorFrames <= 0 && audio.Capture.FallbackMonitorFrames <= 0)
                    {
                        lines.Add("Audio monitor fault: program/master PCM is present but the MON bus has no routed PCM.");
                    }

                    if (audio.MonitorEnabled && audio.Capture.RoutedMonitorFrames > 0 && audio.MonitorFramesPlayed <= 0)
                    {
                        lines.Add("Audio monitor fault: MON bus has PCM but the monitor output reported no playback frames.");
                    }
                }

                if (mediaCore.Senders.ActiveSenderCount > 0)
                {
                    lines.Add(
                        $"Output senders: {mediaCore.Senders.Status}; active {mediaCore.Senders.ActiveSenderCount}");
                    if (audio.Capture.RoutedMasterFrames > 0)
                    {
                        foreach (var sender in mediaCore.Senders.Destinations.Where(sender =>
                                     sender.Status == "live" && sender.AudioFramesSent <= 0))
                        {
                            lines.Add($"Output audio fault: {sender.Destination} is live but has accepted no program-audio frames.");
                        }
                    }
                }

                if (mediaCore.Recording is { Status: "recording" or "warning" } recording)
                {
                    var proof = recording.Proof;
                    if (proof is not null)
                    {
                        lines.Add(
                            $"Recording audio proof: packets {proof.AudioPacketsObserved}; samples {proof.AudioSampleCount}; {proof.AudioChannels} ch @ {proof.AudioSampleRate} Hz");
                    }

                    if (audio.Capture.RoutedMasterFrames > 0 && proof is null)
                    {
                        lines.Add("Recording audio fault: active recording has no audio proof telemetry from the native writer.");
                    }
                    else if (audio.Capture.RoutedMasterFrames > 0 &&
                             (proof?.AudioSampleCount ?? 0) <= 0 &&
                             (proof?.AudioPacketsObserved ?? 0) <= 0)
                    {
                        lines.Add("Recording audio fault: program/master PCM is present but the active recording has no audio samples.");
                    }
                }
            }
        }
        else
        {
            lines.Add("Media core snapshot: unavailable (engine off).");
        }

        if (crashEvents.Count > 0)
        {
            var latest = crashEvents[^1];
            var exit = latest.ExitCode?.ToString(CultureInfo.InvariantCulture) ?? "unknown";
            lines.Add($"Latest crash: exit {exit} at {latest.At}");
        }

        return lines.ToArray();
    }

    /// <summary>
    /// Redacts secrets embedded in an endpoint/URL. Mirrors redactEndpoint in
    /// src/engine/supportBundle.ts (credentials and known secret query params).
    /// </summary>
    public static string RedactEndpoint(string? endpoint)
    {
        if (string.IsNullOrEmpty(endpoint))
        {
            return string.Empty;
        }

        if (Uri.TryCreate(endpoint, UriKind.Absolute, out var uri))
        {
            var builder = new UriBuilder(uri);
            if (!string.IsNullOrEmpty(builder.UserName))
            {
                builder.UserName = "redacted";
            }

            if (!string.IsNullOrEmpty(builder.Password))
            {
                builder.Password = "redacted";
            }

            if (!string.IsNullOrEmpty(builder.Query))
            {
                builder.Query = SecretQueryParam.Replace(
                    builder.Query.TrimStart('?'),
                    m => $"{m.Groups[1].Value}=redacted");
            }

            return builder.Uri.ToString();
        }

        return SecretQueryParam.Replace(endpoint, m => $"{m.Groups[1].Value}=redacted");
    }

    /// <summary>
    /// Mirrors redactSecret in src/engine/supportBundle.ts — never emits the key.
    /// </summary>
    public static string RedactSecret(string? value) =>
        string.IsNullOrEmpty(value) ? "absent" : "present-redacted";
}
