using CoreVideoPro.MediaCore.Models;
using System.Text.Json;

namespace CoreVideoPro.MediaCore.Services;

public static class SyntheticMediaCore
{
    private static readonly NativeMediaCoreOutputProfile DefaultOutputProfile = new()
    {
        ProfileId = "1080p60",
        Resolution = "1920x1080",
        Width = 1920,
        Height = 1080,
        Fps = 60,
        TargetBitrateMbps = 8
    };

    private static readonly NativeMediaCoreColorGrade NeutralGrade = new()
    {
        Lut = "none",
        Exposure = 0,
        Contrast = 0,
        Saturation = 0,
        Temperature = 0
    };

    public static readonly NativeMediaCoreProfile SyntheticProfile = new()
    {
        Name = "CoreVideo synthetic core (Node stub)",
        Renderer = "vulkan",
        MaxProgramResolution = "3840x2160",
        MaxProgramFps = 60,
        MaxParticipantFeeds = 8,
        MaxIsoRecordings = 8,
        Capabilities =
        [
            "zoom-raw-video",
            "zoom-raw-audio",
            "gpu-compositor",
            "scene-graph-rendering",
            "dynamic-overlays",
            "chroma-key",
            "smart-framing",
            "audio-mixer",
            "program-recording",
            "iso-recording",
            "rtmp-output",
            "ndi-output",
            "srt-output",
            "srt-ingest",
            "webrtc-output",
            "decklink-capture",
            "aja-capture"
        ]
    };

    public static NativeMediaCoreStateSnapshot SynthesizeSnapshot(
        IReadOnlyList<NativeMediaCoreCommand> commands,
        double elapsedMs,
        int frameNumber)
    {
        var sceneGraph = commands.FirstOrDefault(command => command.Type == "load-scene-graph");
        var outputCommand = commands.FirstOrDefault(command => command.Type == "start-program-output");
        var recordingCommand = commands.FirstOrDefault(command => command.Type == "start-recording-session");
        var sceneId = TryGetString(sceneGraph, "sceneId") ?? "idle";
        var routeCount = TryGetRouteCount(sceneGraph);
        var outputs = TryGetStringArray(outputCommand, "destinations");
        var isoParticipantIds = TryGetStringArray(outputCommand, "isoParticipantIds");
        var recordingOutputProfile = TryGetObject(outputCommand, "recordingOutputProfile") ??
            TryGetObject(recordingCommand, "renderProfile");
        var recordingCodec = NormalizeVideoCodec(TryGetString(recordingOutputProfile, "codec"));
        var sourceCount = commands.Count(command => command.Type == "set-zoom-source-roster");
        var hasProgramOutput = outputCommand is not null;
        var programFrameCount = hasProgramOutput ? frameNumber : 0;

        var renderPlan = new NativeMediaCoreRenderPlan
        {
            RenderPlanId = $"{sceneId}:{routeCount}:{frameNumber}",
            SceneId = sceneId,
            OutputProfile = DefaultOutputProfile,
            ColorGrade = NeutralGrade,
            SourceCount = sourceCount,
            ResolvedRouteCount = routeCount,
            Layers = [],
            Routes = [],
            Warnings = []
        };

        var compositor = new NativeMediaCoreCompositorState
        {
            Status = programFrameCount > 0 ? "live" : "idle",
            RenderPlanId = renderPlan.RenderPlanId,
            ProgramFrameCount = programFrameCount,
            DroppedFrameCount = 0,
            DegradedFrameCount = 0
        };

        var encoderSession = new NativeMediaCoreEncoderSession
        {
            Status = hasProgramOutput ? "encoding" : "idle",
            RenderPlanId = renderPlan.RenderPlanId,
            ProgramFrameCount = programFrameCount,
            Targets = [],
            Lifecycle = new NativeMediaCoreEncoderLifecycle
            {
                Status = hasProgramOutput ? "encoding" : "idle",
                LastTransition = hasProgramOutput
                    ? "Program output encoder session started."
                    : "Encoder idle."
            },
            Warnings = []
        };

        var outputSenderSession = new NativeMediaCoreOutputSenderSession
        {
            Status = outputs.Contains("rtmp") ? "live" : "idle",
            ActiveSenderCount = outputs.Contains("rtmp") ? 1 : 0,
            Senders = outputs.Contains("rtmp")
                ?
                [
                    new NativeMediaCoreOutputSender
                    {
                        SenderId = "rtmp:program",
                        Destination = "rtmp",
                        Status = "live",
                        FramesSent = programFrameCount,
                        RetryCount = 0,
                        LatencyMs = 2100,
                        BitrateMbps = 6
                    }
                ]
                : [],
            Warnings = []
        };

        NativeMediaCoreRecordingSession? recording = null;
        if (recordingCommand is not null)
        {
            recording = new NativeMediaCoreRecordingSession
            {
                SessionId = TryGetString(recordingCommand, "sessionId") ?? "recording",
                Active = true,
                Status = "recording",
                WriterStatus = "writing",
                StartedAtMs = Math.Max(0, elapsedMs - 1000),
                ElapsedMs = Math.Max(0, elapsedMs - 1000),
                TargetFolder = TryGetString(recordingCommand, "targetFolder") ?? "Recordings",
                FilenamePrefix = TryGetString(recordingCommand, "filenamePrefix") ?? "program",
                Format = TryGetString(recordingCommand, "format") ?? "mp4",
                Quality = TryGetString(recordingCommand, "quality") ?? "high",
                Encoder = new NativeMediaCoreRecordingEncoder
                {
                    Codec = recordingCodec,
                    HardwareAccelerated = true,
                    TargetBitrateMbps = 18
                },
                EstimatedDiskRateMBps = 4.99,
                ProgramPath = $"{TryGetString(recordingCommand, "targetFolder") ?? "Recordings"}/{TryGetString(recordingCommand, "filenamePrefix") ?? "program"}-program-0.mp4",
                Streams = [],
                TotalFramesWritten = programFrameCount,
                TotalDroppedFrames = 0,
                TotalBytesWritten = programFrameCount > 0 ? 4096 : 0
            };
        }

        var outputHealth = BuildOutputHealth(outputs, outputSenderSession, recording, encoderSession.Warnings);
        var programFrame = programFrameCount > 0
            ? new NativeMediaCoreProgramFrame
            {
                FrameNumber = programFrameCount,
                TimestampMs = elapsedMs,
                RenderPlanId = renderPlan.RenderPlanId,
                SceneId = sceneId,
                Width = DefaultOutputProfile.Width,
                Height = DefaultOutputProfile.Height,
                Fps = DefaultOutputProfile.Fps,
                LayerCount = renderPlan.Layers.Count,
                ColorGrade = NeutralGrade,
                Health = "live"
            }
            : null;
        var programFramePreview = SynthesizeProgramFramePreview(programFrame, programFrameCount);

        var sourceSnapshot = new NativeMediaCoreFrameSourceSnapshot
        {
            AdapterId = "synthetic-core",
            Kind = "zoom-sdk",
            Status = sourceCount > 0 ? "subscribed" : "idle",
            SubscribedSourceCount = sourceCount,
            LiveFrameCount = sourceCount,
            StaleFrameCount = 0,
            DroppedFrameCount = 0,
            LowResolutionFrameCount = 0,
            LastFrameTimestampMs = sourceCount > 0 ? elapsedMs : null,
            Warnings = []
        };

        var audioMixCommand = commands.FirstOrDefault(command => command.Type == "sync-participant-audio-mix");
        var audioMonitorCommand = commands.FirstOrDefault(command => command.Type == "sync-audio-monitor");
        var limiterEnabled = TryGetBool(audioMixCommand, "limiterEnabled") ?? true;
        var monitorEnabled = TryGetBool(audioMonitorCommand, "enabled") ?? false;
        var monitorDeviceId = TryGetString(audioMonitorCommand, "deviceId");
        var monitorDeviceName = TryGetString(audioMonitorCommand, "deviceName");
        var monitorVolume = TryGetDouble(audioMonitorCommand, "volume") ?? 0;
        var audioMixSession = new NativeMediaCoreAudioMixSession
        {
            Status = "idle",
            MasterLevel = 0,
            LoudnessLufs = -60,
            LimiterEnabled = limiterEnabled,
            LimiterActive = false,
            MixedFrameCount = 0,
            MonitorEnabled = monitorEnabled,
            MonitorStatus = monitorEnabled && string.IsNullOrWhiteSpace(monitorDeviceId)
                ? "missing-device"
                : monitorEnabled
                    ? "armed"
                    : "muted",
            MonitorDeviceId = monitorDeviceId,
            MonitorDeviceName = monitorDeviceName,
            MonitorVolume = monitorVolume,
            MonitorFramesPlayed = 0,
            Participants = [],
            Summary = "Audio mix idle",
            Warnings = []
        };

        var captionTrack = new NativeMediaCoreCaptionTrack
        {
            Enabled = false,
            Status = "idle",
            LatencyMs = 0,
            Warnings = []
        };

        var brandKit = new NativeMediaCoreBrandKit
        {
            Name = "Default",
            LogoText = "CV",
            LogoAssetId = null,
            LogoAssetName = null,
            LogoAssetPath = null,
            BrandColor = "#111111",
            AccentColor = "#3366ff",
            BackgroundColor = "#000000",
            FontFamily = "Inter",
            LowerThirdStyle = "solid",
            CaptionStyle = "medium sentence captions",
            DefaultOverlayBehavior = "all-off",
            AppliedOverlayCount = 0,
            Summary = "Brand kit idle",
            Warnings = []
        };

        var diagnostics = new NativeMediaCoreDiagnosticsSnapshot
        {
            GeneratedAtMs = elapsedMs,
            SceneId = sceneId,
            RouteCount = routeCount,
            FrameCount = sourceCount,
            ProgramFrameCount = programFrameCount,
            Outputs = outputs,
            OutputProfile = DefaultOutputProfile,
            OutputHealth = outputHealth,
            OutputSenderSession = outputSenderSession,
            SourceSnapshot = sourceSnapshot,
            RenderPlan = renderPlan,
            Compositor = compositor,
            ProgramFrame = programFrame,
            ProgramFramePreview = programFramePreview,
            ProgramTransport = new NativeMediaCoreProgramFrameTransport
            {
                Status = programFrame is null ? "idle" : "publishing",
                FrameNumber = programFrame?.FrameNumber,
                RenderPlanId = programFrame?.RenderPlanId,
                TimestampMs = programFrame?.TimestampMs,
                LatencyMs = 0
            },
            EncoderSession = encoderSession,
            Recording = recording,
            AudioMixSession = audioMixSession,
            CaptionTrack = captionTrack,
            BrandKit = brandKit,
            OperatorActions = [],
            EventLog = [],
            Warnings = [],
            LastCommandTypes = commands.Select(command => command.Type).ToList()
        };

        return new NativeMediaCoreStateSnapshot
        {
            SceneId = sceneId,
            RouteCount = routeCount,
            FrameCount = sourceCount,
            Frames = [],
            SourceSnapshot = sourceSnapshot,
            ProgramFrame = programFrame,
            ProgramFramePreview = programFramePreview,
            ProgramFrameCount = programFrameCount,
            ProgramTransport = diagnostics.ProgramTransport,
            Compositor = compositor,
            ParticipantTransformCount = commands.Count(command => command.Type == "set-participant-transform"),
            OverlayCount = commands.Count(command => command.Type == "set-overlay-asset"),
            Outputs = outputs,
            IsoParticipantIds = isoParticipantIds,
            OutputProfile = DefaultOutputProfile,
            OutputHealth = outputHealth,
            OutputSenderSession = outputSenderSession,
            SourceCount = sourceCount,
            ResolvedRouteCount = routeCount,
            RenderPlan = renderPlan,
            EncoderSession = encoderSession,
            Recording = recording,
            AudioMixSession = audioMixSession,
            CaptionTrack = captionTrack,
            BrandKit = brandKit,
            OperatorActions = [],
            EventLog = [],
            Diagnostics = diagnostics,
            LastCommandTypes = diagnostics.LastCommandTypes,
            Warnings = [],
            MeetingState = sourceCount > 0 || programFrameCount > 0 ? "in_meeting" : null,
            BreakoutRoomId = sourceCount > 0 || programFrameCount > 0 ? "main" : null,
            BreakoutRoomName = sourceCount > 0 || programFrameCount > 0 ? "Main room" : null
        };
    }

    private static NativeMediaCoreProgramFramePreview? SynthesizeProgramFramePreview(
        NativeMediaCoreProgramFrame? programFrame,
        int frameNumber)
    {
        if (programFrame is null || frameNumber <= 0)
        {
            return null;
        }

        const int width = 320;
        const int height = 180;
        var pixels = new byte[width * height * 4];
        for (var y = 0; y < height; y++)
        {
            for (var x = 0; x < width; x++)
            {
                var offset = (y * width + x) * 4;
                pixels[offset] = (byte)((x + frameNumber * 7) % 256);
                pixels[offset + 1] = (byte)((y + frameNumber * 11) % 256);
                pixels[offset + 2] = (byte)(((x / 8) + (y / 8) + frameNumber * 13) % 256);
                pixels[offset + 3] = 255;
            }
        }

        return new NativeMediaCoreProgramFramePreview
        {
            FrameNumber = frameNumber,
            Width = width,
            Height = height,
            RenderPlanId = programFrame.RenderPlanId,
            Renderer = "software",
            Health = programFrame.Health,
            PixelFormat = "bgra",
            Bgra = pixels,
            BgraBase64 = Convert.ToBase64String(pixels)
        };
    }

    private static IReadOnlyList<NativeMediaCoreOutputHealth> BuildOutputHealth(
        IReadOnlyList<string> outputs,
        NativeMediaCoreOutputSenderSession senderSession,
        NativeMediaCoreRecordingSession? recording,
        IReadOnlyList<string> encoderWarnings)
    {
        var health = new List<NativeMediaCoreOutputHealth>();
        foreach (var destination in outputs)
        {
            if (destination == "recording")
            {
                var status = recording?.Status switch
                {
                    "failed" => "failed",
                    "warning" => "warning",
                    _ when recording?.Active == true => "live",
                    _ => "idle"
                };
                health.Add(new NativeMediaCoreOutputHealth
                {
                    Destination = destination,
                    Status = status,
                    Message = recording?.Error ?? recording?.Warning ?? recording?.ProgramPath ?? "Recording idle.",
                    DroppedFrames = recording?.TotalDroppedFrames ?? 0
                });
                continue;
            }

            var sender = senderSession.Senders.FirstOrDefault(item => item.Destination == destination);
            health.Add(new NativeMediaCoreOutputHealth
            {
                Destination = destination,
                Status = sender?.Status switch
                {
                    "failed" => "failed",
                    "warning" => "warning",
                    "live" or "starting" => "live",
                    _ => "idle"
                },
                Message = FormatOutputSenderHealthMessage(destination, sender),
                DroppedFrames = 0
            });
        }

        foreach (var warning in encoderWarnings)
        {
            health.Add(new NativeMediaCoreOutputHealth
            {
                Destination = "recording",
                Status = "warning",
                Message = warning,
                DroppedFrames = 0
            });
        }

        return health;
    }

    private static string FormatOutputSenderHealthMessage(string destination, NativeMediaCoreOutputSender? sender)
    {
        if (sender is null)
        {
            return $"{destination.ToUpperInvariant()} sender idle.";
        }

        var parts = new[] { sender.Warning, sender.LastError, sender.LastResultCode, sender.RuntimeDetail }
            .Where(static part => !string.IsNullOrWhiteSpace(part))
            .Distinct(StringComparer.Ordinal)
            .ToList();
        return parts.Count > 0
            ? string.Join(" ", parts)
            : $"{destination.ToUpperInvariant()} sender {sender.Status}.";
    }

    private static int TryGetRouteCount(NativeMediaCoreCommand? sceneGraph)
    {
        if (sceneGraph?.ExtensionData is null ||
            !sceneGraph.ExtensionData.TryGetValue("routes", out var routesElement) ||
            routesElement.ValueKind != System.Text.Json.JsonValueKind.Array)
        {
            return 0;
        }

        return routesElement.GetArrayLength();
    }

    private static string? TryGetString(NativeMediaCoreCommand? command, string propertyName)
    {
        if (command?.ExtensionData is null ||
            !command.ExtensionData.TryGetValue(propertyName, out var value) ||
            value.ValueKind != System.Text.Json.JsonValueKind.String)
        {
            return null;
        }

        return value.GetString();
    }

    private static string? TryGetString(JsonElement? element, string propertyName)
    {
        if (element is not { ValueKind: System.Text.Json.JsonValueKind.Object } value ||
            !value.TryGetProperty(propertyName, out var property) ||
            property.ValueKind != System.Text.Json.JsonValueKind.String)
        {
            return null;
        }

        return property.GetString();
    }

    private static JsonElement? TryGetObject(NativeMediaCoreCommand? command, string propertyName)
    {
        if (command?.ExtensionData is null ||
            !command.ExtensionData.TryGetValue(propertyName, out var value) ||
            value.ValueKind != System.Text.Json.JsonValueKind.Object)
        {
            return null;
        }

        return value;
    }

    private static string NormalizeVideoCodec(string? codec)
    {
        var normalized = codec?.Trim().ToLowerInvariant().Replace("hevc", "h265");
        return normalized is "h264" or "h265" or "av1" ? normalized : "h264";
    }

    private static IReadOnlyList<string> TryGetStringArray(NativeMediaCoreCommand? command, string propertyName)
    {
        if (command?.ExtensionData is null ||
            !command.ExtensionData.TryGetValue(propertyName, out var value) ||
            value.ValueKind != System.Text.Json.JsonValueKind.Array)
        {
            return [];
        }

        return value.EnumerateArray()
            .Where(element => element.ValueKind == System.Text.Json.JsonValueKind.String)
            .Select(element => element.GetString()!)
            .ToList();
    }

    private static bool? TryGetBool(NativeMediaCoreCommand? command, string propertyName)
    {
        if (command?.ExtensionData is null ||
            !command.ExtensionData.TryGetValue(propertyName, out var value))
        {
            return null;
        }

        return value.ValueKind == System.Text.Json.JsonValueKind.True
            ? true
            : value.ValueKind == System.Text.Json.JsonValueKind.False
                ? false
            : null;
    }

    private static double? TryGetDouble(NativeMediaCoreCommand? command, string propertyName)
    {
        if (command?.ExtensionData is null ||
            !command.ExtensionData.TryGetValue(propertyName, out var value) ||
            value.ValueKind != JsonValueKind.Number)
        {
            return null;
        }

        return value.GetDouble();
    }
}
