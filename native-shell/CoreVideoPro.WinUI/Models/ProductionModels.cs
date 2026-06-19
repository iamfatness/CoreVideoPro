using CommunityToolkit.Mvvm.ComponentModel;

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

public partial class CaptureDevice : ObservableObject
{
    public required string Id { get; init; }
    public required string Vendor { get; init; }
    public required string Name { get; init; }
    public required IReadOnlyList<CaptureDeviceInput> Inputs { get; init; }
    public required string SelectedInputId { get; set; }
    public required int Width { get; init; }
    public required int Height { get; init; }
    public required int FrameRate { get; init; }

    [ObservableProperty]
    private CaptureConnectionState _connectionState;

    [ObservableProperty]
    private bool _signalPresent;

    public int AudioSyncOffsetMs { get; set; }

    public string ResolutionLabel => $"{Width}x{Height}";

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

    public bool IsConnected => ConnectionState == CaptureConnectionState.Connected;

    public bool ShowConnectButton => !IsConnected;

    partial void OnConnectionStateChanged(CaptureConnectionState value)
    {
        OnPropertyChanged(nameof(IsConnected));
        OnPropertyChanged(nameof(ConnectionLabel));
        OnPropertyChanged(nameof(ShowConnectButton));
    }

    partial void OnSignalPresentChanged(bool value) => OnPropertyChanged(nameof(SignalLabel));
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
    public required bool IsSelected { get; init; }
}

public sealed class ParticipantAudioMix
{
    public required string ParticipantId { get; init; }
    public required int OutputLevel { get; init; }
    public required double GainDb { get; init; }
    public double ManualGainDb { get; set; }
    public required bool NoiseSuppression { get; init; }
    public bool Muted { get; set; }
    public required string Status { get; init; }
}

public sealed class AudioMixState
{
    public required IReadOnlyList<ParticipantAudioMix> Participants { get; init; }
    public required double LoudnessLufs { get; init; }
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

    public string DurationLabel =>
        DurationMs is { } ms ? $"{Math.Round(ms / 100.0) / 10.0:0.#}s" : string.Empty;

    public string DetailLabel =>
        string.Join(" - ", new[] { Kind, FileType, RelativePath }.Where(part => !string.IsNullOrWhiteSpace(part)));

    public string SelectionLabel => IsSelected ? "Selected" : "Select";
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
        IReadOnlyList<Scene> scenes)
    {
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
        if (screenSharer is not null)
        {
            return new AutoProductionState
            {
                RecommendedSceneId = "speaker-slides",
                Confidence = 88,
                Reason = $"{screenSharer.Name} is sharing — speaker + slides fits best.",
                Action = "hold"
            };
        }

        if (participants.Count >= 4)
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
                    NoiseSuppression = prior.NoiseSuppression,
                    Muted = prior.Muted,
                    Status = prior.Status
                };
            }

            return new ParticipantAudioMix
            {
                ParticipantId = participant.Id,
                OutputLevel = participant.AudioLevel,
                GainDb = 0,
                ManualGainDb = 0,
                NoiseSuppression = true,
                Muted = participant.IsMuted,
                Status = participant.IsMuted ? "muted" : "balanced"
            };
        }).ToList();
    }

    public static string BuildAudioMixSummary(IReadOnlyList<Participant> participants) =>
        participants.Count == 0
            ? "No meeting audio"
            : $"{participants.Count} sources in mix";

    public static string RecommendedSceneName(IReadOnlyList<Scene> scenes, string sceneId) =>
        scenes.FirstOrDefault(scene => scene.Id == sceneId)?.Name ?? "—";

    public static string RecommendedLayout(IReadOnlyList<Scene> scenes, string sceneId) =>
        scenes.FirstOrDefault(scene => scene.Id == sceneId)?.Layout ?? "—";
}
