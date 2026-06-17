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
    public required string BrandColor { get; init; }
    public required string AccentColor { get; init; }
    public required string BackgroundColor { get; init; }
    public required string FontFamily { get; init; }
    public required string LowerThirdStyle { get; init; }

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

    public string DurationLabel =>
        DurationMs is { } ms ? $"{Math.Round(ms / 100.0) / 10.0:0.#}s" : string.Empty;
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

public static class DemoProduction
{
    public static IReadOnlyList<Participant> Participants { get; } =
    [
        new()
        {
            Id = "p1",
            Name = "Sophia Martinez",
            Title = "Host",
            Role = ParticipantRole.Host,
            BreakoutRoomId = "main",
            BreakoutRoomName = "Main room",
            IsActiveSpeaker = false,
            IsMuted = false,
            IsScreenSharing = false,
            AudioLevel = 54,
            Health = FeedHealth.Live
        },
        new()
        {
            Id = "p2",
            Name = "David Chen",
            Title = "Chief Product Officer",
            Role = ParticipantRole.Presenter,
            BreakoutRoomId = "main",
            BreakoutRoomName = "Main room",
            IsActiveSpeaker = true,
            IsMuted = false,
            IsScreenSharing = true,
            AudioLevel = 82,
            Health = FeedHealth.Live
        },
        new()
        {
            Id = "p3",
            Name = "Jeremy Collins",
            Title = "Engineering Panelist",
            Role = ParticipantRole.Panelist,
            BreakoutRoomId = "main",
            BreakoutRoomName = "Main room",
            IsActiveSpeaker = false,
            IsMuted = false,
            IsScreenSharing = false,
            AudioLevel = 38,
            Health = FeedHealth.Live
        },
        new()
        {
            Id = "p4",
            Name = "Ava Patel",
            Title = "Customer Panelist",
            Role = ParticipantRole.Panelist,
            BreakoutRoomId = "main",
            BreakoutRoomName = "Main room",
            IsActiveSpeaker = false,
            IsMuted = false,
            IsScreenSharing = false,
            AudioLevel = 34,
            Health = FeedHealth.Live
        },
        new()
        {
            Id = "p5",
            Name = "Michael Thompson",
            Title = "Attendee",
            Role = ParticipantRole.Guest,
            BreakoutRoomId = "main",
            BreakoutRoomName = "Main room",
            IsActiveSpeaker = false,
            IsMuted = true,
            IsScreenSharing = false,
            AudioLevel = 8,
            Health = FeedHealth.Live
        }
    ];

    public static IReadOnlyList<Scene> Scenes { get; } =
    [
        new() { Id = "intro", Name = "Intro", Layout = "host-focus", Automation = "Host lower-third + countdown", DurationLabel = "0:30" },
        new() { Id = "interview", Name = "Interview", Layout = "two-up", Automation = "Two-up speaker hold", DurationLabel = "4:00" },
        new() { Id = "speaker-slides", Name = "Speaker + Slides", Layout = "speaker-slides", Automation = "Presenter priority + captions", DurationLabel = "12:00" },
        new() { Id = "panel", Name = "Panel", Layout = "smart-grid", Automation = "Active speaker plus gallery", DurationLabel = "8:00" },
        new() { Id = "closing", Name = "Closing", Layout = "outro", Automation = "Host outro + CTA", DurationLabel = "1:00" }
    ];

    public static IReadOnlyList<CaptureDevice> CaptureDevices { get; } =
    [
        new()
        {
            Id = "decklink-1",
            Vendor = "blackmagic",
            Name = "DeckLink Mini Recorder 4K",
            Inputs =
            [
                new() { Id = "sdi-1", Label = "SDI 1" },
                new() { Id = "hdmi-1", Label = "HDMI" }
            ],
            SelectedInputId = "sdi-1",
            Width = 1920,
            Height = 1080,
            FrameRate = 60,
            ConnectionState = CaptureConnectionState.Connected,
            SignalPresent = true,
            AudioSyncOffsetMs = 0
        },
        new()
        {
            Id = "aja-io-1",
            Vendor = "aja",
            Name = "AJA Io 4K Plus",
            Inputs =
            [
                new() { Id = "sdi-1", Label = "SDI 1" },
                new() { Id = "sdi-2", Label = "SDI 2" }
            ],
            SelectedInputId = "sdi-1",
            Width = 1920,
            Height = 1080,
            FrameRate = 30,
            ConnectionState = CaptureConnectionState.Detected,
            SignalPresent = false,
            AudioSyncOffsetMs = 0
        },
        new()
        {
            Id = "uvc-webcam-1",
            Vendor = "uvc",
            Name = "Logitech Brio 4K",
            Inputs =
            [
                new() { Id = "usb-1", Label = "USB" }
            ],
            SelectedInputId = "usb-1",
            Width = 1920,
            Height = 1080,
            FrameRate = 30,
            ConnectionState = CaptureConnectionState.Detected,
            SignalPresent = true,
            AudioSyncOffsetMs = 0
        }
    ];

    public static IReadOnlyList<GraphicOverlay> Graphics { get; } =
    [
        new() { Id = "brand-bug", Name = "Brand bug", Kind = "bug", Position = "top-right", Accent = "#44c1a1", Enabled = true },
        new() { Id = "live-banner", Name = "Live banner", Kind = "banner", Position = "bottom-right", Accent = "#ec5757", Enabled = false },
        new() { Id = "question-cta", Name = "Question CTA", Kind = "cta", Position = "center", Accent = "#f0a85c", Enabled = false }
    ];

    public static BrandKit BrandKit { get; } = new()
    {
        Name = "CoreVideo",
        LogoText = "CoreVideo",
        BrandColor = "#44c1a1",
        AccentColor = "#f0a85c",
        BackgroundColor = "#0c1118",
        FontFamily = "Inter",
        LowerThirdStyle = "gradient"
    };

    public static CaptionStyle CaptionStyle { get; } = new()
    {
        FontSize = "medium",
        TextColor = "#f7fbf8",
        BackgroundOpacity = 70,
        Uppercase = false
    };

    public static IReadOnlyList<CaptionTranscriptEntry> CaptionTranscript { get; } =
    [
        new()
        {
            Id = "cc-1",
            SpeakerName = "Sophia Martinez",
            Role = "Host",
            Text = "Welcome to the Q2 Product Update.",
            Confidence = 96
        },
        new()
        {
            Id = "cc-2",
            SpeakerName = "David Chen",
            Role = "Presenter",
            Text = "Let me walk through what we are building next.",
            Confidence = 93
        }
    ];

    public static AudioMixState AudioMix { get; } = new()
    {
        Participants =
        [
            new() { ParticipantId = "p1", OutputLevel = 54, GainDb = 0, ManualGainDb = 0, NoiseSuppression = true, Status = "balanced" },
            new() { ParticipantId = "p2", OutputLevel = 82, GainDb = 2, ManualGainDb = 0, NoiseSuppression = true, Status = "boosting" },
            new() { ParticipantId = "p3", OutputLevel = 38, GainDb = -1, ManualGainDb = 0, NoiseSuppression = true, Status = "balanced" },
            new() { ParticipantId = "p4", OutputLevel = 34, GainDb = -1, ManualGainDb = 0, NoiseSuppression = true, Status = "balanced" },
            new() { ParticipantId = "p5", OutputLevel = 8, GainDb = -6, ManualGainDb = 0, NoiseSuppression = true, Muted = true, Status = "muted" }
        ],
        LoudnessLufs = -16,
        LimiterActive = false,
        Summary = "Smart audio leveling ready"
    };

    public static ColorGrade ColorGrade { get; } = new()
    {
        Lut = "none",
        Exposure = 0,
        Contrast = 0,
        Saturation = 0,
        Temperature = 0
    };

    public static IReadOnlyList<MediaAsset> MediaBin { get; } =
    [
        new() { Id = "stinger-1", Name = "Brand stinger", Kind = "stinger", DurationMs = 900 },
        new() { Id = "lower-third-1", Name = "Host lower-third", Kind = "lower-third" },
        new() { Id = "audio-bed-1", Name = "Intro bed", Kind = "audio-bed", DurationMs = 30000 }
    ];

    public static AutoProductionState AutoProduction { get; } = new()
    {
        RecommendedSceneId = "speaker-slides",
        Confidence = 92,
        Reason = "David Chen is sharing slides, so keep speaker plus slides live.",
        Action = "hold"
    };

    public static string MagicSceneStatus { get; } =
        "Ready to rebuild from 7 participants, 1 presenter, screen share active";

    public static ProductionMode Mode { get; set; } = ProductionMode.SetAndForget;

    public static (string Id, string Name) CurrentRoom()
    {
        var host = Participants.FirstOrDefault(p => p.Role == ParticipantRole.Host);
        return host is not null
            ? (host.BreakoutRoomId, host.BreakoutRoomName)
            : ("main", "Main room");
    }

    public static IReadOnlyList<Participant> VideoParticipantsInRoom(string roomId) =>
        Participants
            .Where(p => p.BreakoutRoomId == roomId && p.Health != FeedHealth.VideoOff)
            .ToList();

    public static string CaptureFleetSummary =>
        $"{CaptureDevices.Count(d => d.ConnectionState == CaptureConnectionState.Connected)} connected · " +
        $"{CaptureDevices.Count(d => d.ConnectionState == CaptureConnectionState.Detected)} detected";

    public static bool DualCaptureLive =>
        CaptureDevices.Count(d => d.ConnectionState == CaptureConnectionState.Connected) >= 2;

    public static string FeedHealthSummary =>
        $"{VideoParticipantsInRoom(CurrentRoom().Id).Count} feeds · all healthy";

    public static IReadOnlyList<FeedHealthRow> FeedHealthRows() =>
        VideoParticipantsInRoom(CurrentRoom().Id).Select(p =>
        {
            var (label, color, detail, attention) = p.Health switch
            {
                FeedHealth.Recovering => ("Reconnecting", "amber", "No frames for 6s", true),
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

    public static string MediaBinSummary =>
        $"{MediaBin.Count} assets · stinger, lower-third, audio bed ready";

    public static IReadOnlyList<MediaBinGroup> MediaBinGroups() =>
        MediaBin
            .GroupBy(asset => asset.Kind)
            .Select(group => new MediaBinGroup
            {
                Kind = group.Key,
                Label = group.Key.Replace('-', ' '),
                Assets = group.ToList()
            })
            .ToList();

    public static string SceneIntelligenceSummary =>
        "Presenter sharing slides · active speaker detected · automation holding current scene";

    public static string RecommendedSceneName =>
        Scenes.FirstOrDefault(s => s.Id == AutoProduction.RecommendedSceneId)?.Name ?? "Speaker + Slides";
}