using CommunityToolkit.Mvvm.ComponentModel;
using CoreVideoPro.MediaCore.Models;

namespace CoreVideoPro.WinUI.Models;

public enum ParticipantRole
{
    Host,
    Guest,
    Presenter,
    Panelist
}

public enum FeedHealth
{
    Live,
    LowResolution,
    Recovering,
    VideoOff
}

public enum ProductionMode
{
    Manual,
    SetAndForget
}

public enum CaptureConnectionState
{
    Disconnected,
    Detected,
    Connected,
    FormatMismatch,
    Error
}

public sealed class Participant
{
    public string Id { get; init; } = string.Empty;
    public string Name { get; init; } = string.Empty;
    public string Title { get; init; } = string.Empty;
    public ParticipantRole Role { get; init; }
    public string BreakoutRoomId { get; init; } = string.Empty;
    public string BreakoutRoomName { get; init; } = string.Empty;
    public bool IsActiveSpeaker { get; init; }
    public bool IsMuted { get; init; }
    public bool IsScreenSharing { get; init; }
    public int AudioLevel { get; init; }
    public FeedHealth Health { get; init; }

    public string RoleBadge => Role switch
    {
        ParticipantRole.Host => "HOST",
        ParticipantRole.Presenter => "SPEAKER",
        ParticipantRole.Guest => "GUEST",
        ParticipantRole.Panelist => "PANEL",
        _ => Role.ToString().ToUpperInvariant()
    };

    public string RoleLabel => Role.ToString();

    public string HealthLabel => Health switch
    {
        FeedHealth.Live => "Live",
        FeedHealth.LowResolution => "Low res",
        FeedHealth.Recovering => "Recovering",
        FeedHealth.VideoOff => "Video off",
        _ => Health.ToString()
    };

    public string Initials
    {
        get
        {
            var parts = Name.Split(' ', StringSplitOptions.RemoveEmptyEntries);
            return string.Concat(parts.Select(part => part[0]));
        }
    }
}

public sealed class Scene
{
    public string Id { get; init; } = string.Empty;
    public string Name { get; init; } = string.Empty;
    public string Layout { get; init; } = string.Empty;
    public string Automation { get; init; } = string.Empty;
    public string DurationLabel { get; init; } = "—";
}

public enum StudioViewMode
{
    Program,
    Preview,
    ProgramPreview,
    Multiview
}

public enum StudioTab
{
    Studio,
    Settings,
    Sources,
    Inputs,
    Routing,
    Overlays,
    Audio,
    Media,
    Automation
}

public sealed class FeedHealthRow
{
    public required string ParticipantId { get; init; }
    public required string Name { get; init; }
    public required string Role { get; init; }
    public required string StatusLabel { get; init; }
    public required string BadgeColor { get; init; }
    public string? Detail { get; init; }
    public bool NeedsAttention { get; init; }
}

public sealed class CaptureDeviceInput
{
    public required string Id { get; init; }
    public required string Label { get; init; }
}

public sealed class AudioCaptureDevice
{
    public required string Id { get; init; }
    public required string NativeDeviceId { get; init; }
    public required string Name { get; init; }
    public string SourceKind { get; init; } = "wasapi-input";
    public string DriverName { get; init; } = "WASAPI";
    public string? LinkedCaptureDeviceId { get; init; }
    public bool IsAvailable { get; init; } = true;

    public bool IsEmbeddedCaptureAudio => SourceKind.Equals("embedded-capture-audio", StringComparison.OrdinalIgnoreCase);

    public string KindLabel => SourceKind switch
    {
        "wasapi-input" => "Mic / line input",
        "asio-input" => "ASIO",
        "embedded-capture-audio" => "Embedded capture audio",
        "local-machine-audio" => "Local machine audio",
        "loopback" => "System loopback",
        _ => SourceKind
    };

    public string DisplayLabel => string.IsNullOrWhiteSpace(DriverName)
        ? $"{Name} - {KindLabel}"
        : $"{Name} - {KindLabel} - {DriverName}";
}

public sealed class AudioRenderDevice
{
    public required string Id { get; init; }
    public required string NativeDeviceId { get; init; }
    public required string Name { get; init; }
}

public partial class CaptureDevice : ObservableObject
{
    public required string Id { get; init; }
    public required string NativeDeviceId { get; init; }
    public required string Vendor { get; init; }
    public required string Name { get; init; }
    public required IReadOnlyList<CaptureDeviceInput> Inputs { get; init; }
    public required string SelectedInputId { get; set; }

    private string? _assignedAudioDeviceId;
    public string? AssignedAudioDeviceId
    {
        get => _assignedAudioDeviceId;
        set => SetProperty(ref _assignedAudioDeviceId, value);
    }

    private string? _assignedAudioDeviceName;
    public string? AssignedAudioDeviceName
    {
        get => _assignedAudioDeviceName;
        set
        {
            if (SetProperty(ref _assignedAudioDeviceName, value))
            {
                OnPropertyChanged(nameof(AssignedAudioLabel));
            }
        }
    }

    private int _width;
    public int Width
    {
        get => _width;
        set
        {
            if (SetProperty(ref _width, value))
            {
                OnPropertyChanged(nameof(ResolutionLabel));
                OnPropertyChanged(nameof(FormatLabel));
            }
        }
    }

    private int _height;
    public int Height
    {
        get => _height;
        set
        {
            if (SetProperty(ref _height, value))
            {
                OnPropertyChanged(nameof(ResolutionLabel));
                OnPropertyChanged(nameof(FormatLabel));
            }
        }
    }

    private int _frameRate;
    public int FrameRate
    {
        get => _frameRate;
        set
        {
            if (SetProperty(ref _frameRate, value))
            {
                OnPropertyChanged(nameof(FormatLabel));
            }
        }
    }

    private int _observedFrameWidth;
    public int ObservedFrameWidth
    {
        get => _observedFrameWidth;
        set
        {
            if (SetProperty(ref _observedFrameWidth, value))
            {
                OnPropertyChanged(nameof(ObservedSignalLabel));
            }
        }
    }

    private int _observedFrameHeight;
    public int ObservedFrameHeight
    {
        get => _observedFrameHeight;
        set
        {
            if (SetProperty(ref _observedFrameHeight, value))
            {
                OnPropertyChanged(nameof(ObservedSignalLabel));
            }
        }
    }

    private int _observedFrameRate;
    public int ObservedFrameRate
    {
        get => _observedFrameRate;
        set
        {
            if (SetProperty(ref _observedFrameRate, value))
            {
                OnPropertyChanged(nameof(ObservedSignalLabel));
            }
        }
    }

    [ObservableProperty]
    private CaptureConnectionState _connectionState;

    [ObservableProperty]
    private bool _signalPresent;

    private int _audioSyncOffsetMs;
    public int AudioSyncOffsetMs
    {
        get => _audioSyncOffsetMs;
        set
        {
            if (SetProperty(ref _audioSyncOffsetMs, value))
            {
                OnPropertyChanged(nameof(AudioSyncLabel));
            }
        }
    }

    public string AudioSyncLabel => AudioSyncOffsetMs == 0
        ? "Audio sync 0 ms"
        : AudioSyncOffsetMs > 0
            ? $"Audio delayed +{AudioSyncOffsetMs} ms"
            : $"Audio advanced {AudioSyncOffsetMs} ms";

    public string AssignedAudioLabel =>
        string.IsNullOrWhiteSpace(AssignedAudioDeviceName)
            ? "Audio source: none"
            : $"Audio source: {AssignedAudioDeviceName}";

    public void NotifyAudioAssignmentChanged()
    {
        OnPropertyChanged(nameof(AssignedAudioDeviceId));
        OnPropertyChanged(nameof(AssignedAudioDeviceName));
        OnPropertyChanged(nameof(AssignedAudioLabel));
    }

    private int EffectiveWidth => Width > 0 ? Width : ObservedFrameWidth;

    private int EffectiveHeight => Height > 0 ? Height : ObservedFrameHeight;

    private int EffectiveFrameRate => FrameRate > 0 ? FrameRate : ObservedFrameRate;

    public string ResolutionLabel => EffectiveWidth > 0 && EffectiveHeight > 0 ? $"{EffectiveWidth}x{EffectiveHeight}" : "Format pending";

    public string FormatLabel =>
        EffectiveWidth > 0 && EffectiveHeight > 0 && EffectiveFrameRate > 0
            ? $"{EffectiveWidth}x{EffectiveHeight} · {EffectiveFrameRate} fps"
            : EffectiveWidth > 0 && EffectiveHeight > 0
                ? $"{EffectiveWidth}x{EffectiveHeight} · fps pending"
                : "Format pending";

    public string ConnectionLabel => ConnectionState switch
    {
        CaptureConnectionState.Connected => "connected",
        CaptureConnectionState.Detected => "detected",
        CaptureConnectionState.Disconnected => "disconnected",
        CaptureConnectionState.FormatMismatch => "format mismatch",
        CaptureConnectionState.Error => "error",
        _ => ConnectionState.ToString().ToLowerInvariant()
    };

    public string SignalLabel => SignalPresent ? "Signal present" : "No signal";

    public string ObservedSignalLabel =>
        ObservedFrameWidth > 0 && ObservedFrameHeight > 0 && ObservedFrameRate > 0
            ? $"Signal {ObservedFrameWidth}x{ObservedFrameHeight} · {ObservedFrameRate} fps"
            : ObservedFrameWidth > 0 && ObservedFrameHeight > 0
                ? $"Signal {ObservedFrameWidth}x{ObservedFrameHeight}"
                : SignalLabel;

    public bool IsConnected => ConnectionState == CaptureConnectionState.Connected;

    public bool ShowConnectButton => !IsConnected;

    public void ApplyFrameTelemetry(int width, int height, int frameRate)
    {
        ApplyObservedFrameTelemetry(width, height, frameRate);

        if (width <= 0 || height <= 0)
        {
            return;
        }

        if (Width <= 0 || Height <= 0 || (FrameRate <= 0 && frameRate > 0))
        {
            ApplyFormatTelemetry(width, height, frameRate);
        }

        SignalPresent = true;
        if (ConnectionState != CaptureConnectionState.Connected)
        {
            ConnectionState = CaptureConnectionState.Connected;
        }
    }

    public void ApplyObservedFrameTelemetry(int width, int height, int frameRate)
    {
        if (width > 0)
        {
            ObservedFrameWidth = width;
            OnPropertyChanged(nameof(ResolutionLabel));
            OnPropertyChanged(nameof(FormatLabel));
        }

        if (height > 0)
        {
            ObservedFrameHeight = height;
            OnPropertyChanged(nameof(ResolutionLabel));
            OnPropertyChanged(nameof(FormatLabel));
        }

        if (frameRate > 0)
        {
            ObservedFrameRate = frameRate;
            OnPropertyChanged(nameof(FormatLabel));
        }
    }

    public void ApplyFormatTelemetry(int width, int height, int frameRate)
    {
        if (width > 0)
        {
            Width = width;
        }

        if (height > 0)
        {
            Height = height;
        }

        if (frameRate > 0)
        {
            FrameRate = frameRate;
        }
    }

    partial void OnConnectionStateChanged(CaptureConnectionState value)
    {
        OnPropertyChanged(nameof(IsConnected));
        OnPropertyChanged(nameof(ConnectionLabel));
        OnPropertyChanged(nameof(ShowConnectButton));
    }

    partial void OnSignalPresentChanged(bool value)
    {
        OnPropertyChanged(nameof(SignalLabel));
        OnPropertyChanged(nameof(ObservedSignalLabel));
    }
}

public partial class GraphicOverlay : ObservableObject
{
    public required string Id { get; init; }
    public required string Name { get; init; }
    public required string Kind { get; init; }
    public required string Position { get; init; }
    public required string Accent { get; init; }

    [ObservableProperty]
    private bool _enabled;

    public string PositionLabel => Position.Replace('-', ' ');

    public string StatusLabel => Enabled ? "On" : "Off";

    partial void OnEnabledChanged(bool value) => OnPropertyChanged(nameof(StatusLabel));
}

public sealed record LowerThirdKeyState
{
    public string SourceId { get; init; } = string.Empty;
    public string SourceName { get; init; } = string.Empty;
    public string Title { get; init; } = string.Empty;
    public string Org { get; init; } = string.Empty;
    public string Position { get; init; } = "lower-left";
    public string Phase { get; init; } = "hidden";
    public bool Enabled { get; init; }

    public bool IsVisible => Enabled && !string.IsNullOrWhiteSpace(SourceName) && Phase != "hidden";

    public string PhaseLabel => Phase switch
    {
        "building-in" => "Building in",
        "building-out" => "Building out",
        "on-air" => "On air",
        _ => "Key off"
    };

    public string SourceLabel => IsVisible ? SourceName : "No source keyed";

    public static LowerThirdKeyState Hidden(string position = "lower-left") =>
        new()
        {
            SourceId = string.Empty,
            SourceName = string.Empty,
            Position = position,
            Phase = "hidden",
            Enabled = false
        };
}

public sealed class BrandKit
{
    public required string Name { get; init; }
    public required string LogoText { get; init; }
    public string? LogoAssetId { get; init; }
    public string? LogoAssetName { get; init; }
    public string? LogoAssetPath { get; init; }
    public required string BrandColor { get; init; }
    public required string AccentColor { get; init; }
    public required string BackgroundColor { get; init; }
    public required string FontFamily { get; init; }
    public required string LowerThirdStyle { get; init; }
    public required string CaptionStyle { get; init; }
    public required string DefaultOverlayBehavior { get; init; }

    public string LogoAssetLabel =>
        string.IsNullOrWhiteSpace(LogoAssetName) ? "No logo asset selected" : LogoAssetName;

    public string Summary =>
        $"{Name} · {FontFamily} · lower-third {LowerThirdStyle} · logo \"{LogoText}\"";
}

public sealed class CaptionStyle
{
    public required string FontSize { get; init; }
    public required string TextColor { get; init; }
    public required int BackgroundOpacity { get; init; }
    public required bool Uppercase { get; init; }

    public string Summary =>
        $"{FontSize} captions · {(Uppercase ? "uppercase" : "sentence case")} · {BackgroundOpacity}% backdrop";
}

public sealed class CaptionTranscriptEntry
{
    public required string Id { get; init; }
    public required string SpeakerName { get; init; }
    public required string Role { get; init; }
    public required string Text { get; init; }
    public required int Confidence { get; init; }
}

public sealed class AudioParticipantRow
{
    public required string Id { get; init; }
    public required string Name { get; init; }
    public required string Subtitle { get; init; }
    public required int OutputLevel { get; init; }
    public required double ManualGainDb { get; init; }
    public required double Pan { get; init; }
    public required double Lufs { get; init; }
    public required double TruePeakDb { get; init; }
    public required bool Muted { get; init; }
    public required string GainLabel { get; init; }
    public required string PanLabel { get; init; }
    public required string LufsLabel { get; init; }
    public required string TruePeakLabel { get; init; }
    public required string BusLabel { get; init; }
    public required string InsertLabel { get; init; }
    public required string MuteButtonLabel { get; init; }
    public required string MuteStateLabel { get; init; }
    public required bool IsSelected { get; init; }
}

public sealed class ParticipantAudioMix
{
    public required string ParticipantId { get; init; }
    public required int OutputLevel { get; init; }
    public required double GainDb { get; init; }
    public double ManualGainDb { get; set; }
    public double Pan { get; set; }
    public bool Solo { get; set; }
    public required bool NoiseSuppression { get; init; }
    public bool Muted { get; set; }
    public required string Status { get; init; }
    public double Lufs { get; set; } = -60;
    public double TruePeakDb { get; set; } = -60;
    public List<string> PluginInserts { get; set; } = [];
}

public sealed class AudioMixState
{
    public required IReadOnlyList<ParticipantAudioMix> Participants { get; init; }
    public required double LoudnessLufs { get; init; }
    public required bool LimiterEnabled { get; init; }
    public required bool LimiterActive { get; init; }
    public required string Summary { get; init; }
}

public sealed class ColorGrade
{
    public required string Lut { get; init; }
    public int Exposure { get; init; }
    public int Contrast { get; init; }
    public int Saturation { get; init; }
    public int Temperature { get; init; }

    public string Summary =>
        $"LUT {Lut} · exposure {FormatAxis(Exposure)} · contrast {FormatAxis(Contrast)} · saturation {FormatAxis(Saturation)}";

    private static string FormatAxis(int value) => value > 0 ? $"+{value}" : value.ToString();
}

public sealed class MediaAsset
{
    public required string Id { get; init; }
    public required string Name { get; init; }
    public required string Kind { get; init; }
    public int? DurationMs { get; init; }
    public string RelativePath { get; init; } = string.Empty;
    public string FilePath { get; init; } = string.Empty;
    public string FileType { get; init; } = string.Empty;
    public bool IsSelected { get; init; }
    public bool IsPlaying { get; init; }

    public string DurationLabel =>
        DurationMs is { } ms ? $"{Math.Round(ms / 100.0) / 10.0:0.#}s" : string.Empty;

    public string DetailLabel =>
        string.Join(" - ", new[] { Kind, FileType, RelativePath }.Where(part => !string.IsNullOrWhiteSpace(part)));

    public string SelectionLabel => IsSelected ? "Selected" : "Select";

    public string PlaybackLabel => IsPlaying ? "Pause" : "Play";
}

public sealed class MediaBinGroup
{
    public required string Kind { get; init; }
    public required string Label { get; init; }
    public required IReadOnlyList<MediaAsset> Assets { get; init; }
}

public sealed class AutoProductionState
{
    public required string RecommendedSceneId { get; init; }
    public required int Confidence { get; init; }
    public required string Reason { get; init; }
    public required string Action { get; init; }
}

/// <summary>Static production templates and UI defaults — no fabricated meeting or device data.</summary>
public static class ProductionCatalog
{
    public static IReadOnlyList<Scene> Scenes { get; } =
    [
        new() { Id = "intro", Name = "Intro", Layout = "host-focus", Automation = "Host lower-third + countdown" },
        new() { Id = "interview", Name = "Interview", Layout = "two-up", Automation = "Two-up speaker hold" },
        new() { Id = "speaker-slides", Name = "Speaker + Slides", Layout = "speaker-slides", Automation = "Presenter priority + captions" },
        new() { Id = "panel", Name = "Panel", Layout = "smart-grid", Automation = "Active speaker plus gallery" },
        new() { Id = "closing", Name = "Closing", Layout = "outro", Automation = "Host outro + CTA" }
    ];

    public static BrandKit BrandKit { get; } = new()
    {
        Name = "CoreVideo",
        LogoText = "CoreVideo",
        BrandColor = "#44c1a1",
        AccentColor = "#f0a85c",
        BackgroundColor = "#0c1118",
        FontFamily = "Inter",
        LowerThirdStyle = "gradient",
        CaptionStyle = "medium sentence captions",
        DefaultOverlayBehavior = "all-off"
    };

    public static CaptionStyle CaptionStyle { get; } = new()
    {
        FontSize = "medium",
        TextColor = "#f7fbf8",
        BackgroundOpacity = 70,
        Uppercase = false
    };

    public static ColorGrade ColorGrade { get; } = new()
    {
        Lut = "none",
        Exposure = 0,
        Contrast = 0,
        Saturation = 0,
        Temperature = 0
    };

    public static IReadOnlyList<GraphicOverlay> DefaultGraphics { get; } = [];
}

public static class ProductionStateHelper
{
    public static string CaptureFleetSummary(IReadOnlyList<CaptureDevice> devices)
    {
        if (devices.Count == 0)
        {
            return "No video capture devices detected";
        }

        var connected = devices.Count(d => d.ConnectionState == CaptureConnectionState.Connected);
        var detected = devices.Count(d => d.ConnectionState == CaptureConnectionState.Detected);
        return $"{connected} connected · {detected} detected";
    }

    public static string CaptureDevicesEmptyGuidance() =>
        "Connect a webcam, capture card, or SDI/HDMI interface, then select Refresh. " +
        "Windows enumerates real devices only — no simulated DeckLink or AJA hardware is shown.";

    public static bool DualCaptureLive(IReadOnlyList<CaptureDevice> devices) =>
        devices.Count(d => d.ConnectionState == CaptureConnectionState.Connected) >= 2;

    public static string FeedHealthSummary(IReadOnlyList<Participant> participants)
    {
        if (participants.Count == 0)
        {
            return "No Zoom feeds — join a meeting";
        }

        var attention = participants.Count(p =>
            p.Health is FeedHealth.Recovering or FeedHealth.LowResolution or FeedHealth.VideoOff);
        return attention == 0
            ? $"{participants.Count} feeds · all healthy"
            : $"{participants.Count} feeds · {attention} need attention";
    }

    public static IReadOnlyList<FeedHealthRow> BuildFeedHealthRows(IReadOnlyList<Participant> participants) =>
        participants.Select(p =>
        {
            var (label, color, detail, attention) = p.Health switch
            {
                FeedHealth.Recovering => ("Reconnecting", "amber", "No frames recently", true),
                FeedHealth.LowResolution => ("Low res", "amber", "Bitrate constrained", true),
                FeedHealth.VideoOff => ("Video off", "red", "Camera disabled", true),
                _ when p.IsMuted => ("Muted", "muted", null, false),
                _ => ("Healthy", "green", null, false)
            };

            return new FeedHealthRow
            {
                ParticipantId = p.Id,
                Name = p.Name,
                Role = p.RoleLabel,
                StatusLabel = label,
                BadgeColor = color,
                Detail = detail,
                NeedsAttention = attention
            };
        }).ToList();

    public static string MediaBinSummary(int assetCount) =>
        assetCount == 0 ? "Media bin is empty" : $"{assetCount} assets in bin";

    public static string CaptionQualitySummary(bool hasCaptions, int entryCount) =>
        hasCaptions
            ? $"{entryCount} caption lines · live"
            : "No captions yet";

    public static AutoProductionState BuildAutomationRecommendation(
        IReadOnlyList<Participant> participants,
        IReadOnlyList<Scene> scenes,
        bool preferScreenShare = true,
        int panelParticipantThreshold = 4)
    {
        panelParticipantThreshold = Math.Clamp(panelParticipantThreshold, 2, 10);
        if (participants.Count == 0)
        {
            return new AutoProductionState
            {
                RecommendedSceneId = "intro",
                Confidence = 0,
                Reason = "Join a Zoom meeting to enable scene recommendations.",
                Action = "idle"
            };
        }

        var screenSharer = participants.FirstOrDefault(p => p.IsScreenSharing);
        if (preferScreenShare && screenSharer is not null)
        {
            return new AutoProductionState
            {
                RecommendedSceneId = "speaker-slides",
                Confidence = 88,
                Reason = $"{screenSharer.Name} is sharing — speaker + slides fits best.",
                Action = "hold"
            };
        }

        if (participants.Count >= panelParticipantThreshold)
        {
            return new AutoProductionState
            {
                RecommendedSceneId = "panel",
                Confidence = 76,
                Reason = $"{participants.Count} participants — panel layout keeps everyone visible.",
                Action = "suggest"
            };
        }

        if (participants.Count >= 2)
        {
            return new AutoProductionState
            {
                RecommendedSceneId = "interview",
                Confidence = 72,
                Reason = $"{participants.Count} participants — two-up interview layout.",
                Action = "suggest"
            };
        }

        return new AutoProductionState
        {
            RecommendedSceneId = "intro",
            Confidence = 64,
            Reason = "Single participant — host-focus intro works well.",
            Action = "suggest"
        };
    }

    /// <summary>
    /// Prefer the deterministic native AI director recommendation (computed in the
    /// C++ media core's Director kernel and surfaced in the snapshot). Falls back to
    /// the count-only C# heuristic when the native recommendation is unavailable
    /// (no meeting joined, native call not yet wired, or an empty payload).
    /// </summary>
    public static AutoProductionState BuildAutomationRecommendation(
        NativeMediaCoreAutoProduction? nativeRecommendation,
        IReadOnlyList<Participant> participants,
        IReadOnlyList<Scene> scenes,
        bool preferScreenShare = true,
        int panelParticipantThreshold = 4)
    {
        if (nativeRecommendation is not null &&
            !string.IsNullOrWhiteSpace(nativeRecommendation.RecommendedSceneId) &&
            participants.Count > 0)
        {
            return new AutoProductionState
            {
                RecommendedSceneId = nativeRecommendation.RecommendedSceneId,
                Confidence = Math.Clamp(nativeRecommendation.Confidence, 0, 100),
                Reason = string.IsNullOrWhiteSpace(nativeRecommendation.Rationale)
                    ? "Native AI director recommendation."
                    : nativeRecommendation.Rationale,
                Action = ResolveNativeRecommendationAction(nativeRecommendation.RuleId)
            };
        }

        return BuildAutomationRecommendation(participants, scenes, preferScreenShare, panelParticipantThreshold);
    }

    private static string ResolveNativeRecommendationAction(string ruleId) => ruleId switch
    {
        "screen-share-priority" => "hold",
        _ => "suggest"
    };

    public static string BuildSceneIntelligenceSummary(
        IReadOnlyList<Participant> participants,
        ProductionMode mode)
    {
        if (participants.Count == 0)
        {
            return "No meeting activity — scene intelligence idle";
        }

        var speaker = participants.FirstOrDefault(p => p.IsActiveSpeaker);
        var sharing = participants.Any(p => p.IsScreenSharing);
        var modeLabel = mode == ProductionMode.SetAndForget ? "automation on" : "manual control";
        var speakerLabel = speaker?.Name ?? "No active speaker";
        var shareLabel = sharing ? "screen share active" : "no screen share";
        return $"{speakerLabel} · {shareLabel} · {modeLabel}";
    }

    public static string BuildMagicSceneStatus(IReadOnlyList<Participant> participants)
    {
        if (participants.Count == 0)
        {
            return "Join a meeting to enable Magic Scene";
        }

        var cameras = participants.Count(p => p.Health != FeedHealth.VideoOff);
        var sharing = participants.Count(p => p.IsScreenSharing);
        return $"Monitoring {participants.Count} participants · {cameras} on camera · {sharing} sharing";
    }

    public static IReadOnlyList<ParticipantAudioMix> BuildAudioMixChannels(
        IReadOnlyList<Participant> participants,
        IReadOnlyDictionary<string, ParticipantAudioMix>? existing = null)
    {
        return participants.Select(participant =>
        {
            if (existing is not null && existing.TryGetValue(participant.Id, out var prior))
            {
                return new ParticipantAudioMix
                {
                    ParticipantId = prior.ParticipantId,
                    OutputLevel = participant.AudioLevel,
                    GainDb = prior.GainDb,
                    ManualGainDb = prior.ManualGainDb,
                    Pan = prior.Pan,
                    Solo = prior.Solo,
                    NoiseSuppression = prior.NoiseSuppression,
                    Muted = prior.Muted,
                    Status = prior.Status,
                    Lufs = EstimateParticipantLufs(participant.AudioLevel, prior.Muted),
                    TruePeakDb = EstimateTruePeakDb(participant.AudioLevel, prior.Muted),
                    PluginInserts = prior.PluginInserts.ToList()
                };
            }

            return new ParticipantAudioMix
            {
                ParticipantId = participant.Id,
                OutputLevel = participant.AudioLevel,
                GainDb = 0,
                ManualGainDb = 0,
                Pan = 0,
                Solo = false,
                NoiseSuppression = true,
                Muted = participant.IsMuted,
                Status = participant.IsMuted ? "muted" : "balanced",
                Lufs = EstimateParticipantLufs(participant.AudioLevel, participant.IsMuted),
                TruePeakDb = EstimateTruePeakDb(participant.AudioLevel, participant.IsMuted),
                PluginInserts = ["Built-in EQ", "Compressor"]
            };
        }).ToList();
    }

    private static double EstimateParticipantLufs(int level, bool muted) =>
        muted || level <= 0 ? -60 : Math.Clamp(-34 + level * 0.22, -60, -10);

    private static double EstimateTruePeakDb(int level, bool muted) =>
        muted || level <= 0 ? -60 : Math.Clamp(-30 + level * 0.28, -60, -1);

    public static string BuildAudioMixSummary(IReadOnlyList<Participant> participants) =>
        participants.Count == 0
            ? "No meeting audio"
            : $"{participants.Count} sources in mix";

    public static string RecommendedSceneName(IReadOnlyList<Scene> scenes, string sceneId) =>
        scenes.FirstOrDefault(scene => scene.Id == sceneId)?.Name ?? "—";

    public static string RecommendedLayout(IReadOnlyList<Scene> scenes, string sceneId) =>
        scenes.FirstOrDefault(scene => scene.Id == sceneId)?.Layout ?? "—";
}
