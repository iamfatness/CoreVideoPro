using CommunityToolkit.Mvvm.ComponentModel;
using CoreVideoPro.MediaCore.Models;
using CoreVideoPro.MediaCore.Services;
using CoreVideoPro.WinUI.Models;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Media;

namespace CoreVideoPro.WinUI.ViewModels;

public sealed partial class TransportViewModel : ObservableObject
{
    [ObservableProperty]
    private bool _magicSceneEnabled = true;

    [ObservableProperty]
    private bool _setAndForgetSelected;

    [ObservableProperty]
    private bool _setAndForgetCanToggle = true;

    [ObservableProperty]
    private string _automationStatusLabel = "On";

    [ObservableProperty]
    private string _programStatValue = "1080p60";

    [ObservableProperty]
    private string _programStatHealthLabel = "Good";

    [ObservableProperty]
    private string _streamStatValue = "Idle";

    [ObservableProperty]
    private string _streamStatHealthLabel = "Idle";

    [ObservableProperty]
    private string _recordStatValue = "Idle";

    [ObservableProperty]
    private string _recordStatHealthLabel = "Idle";

    [ObservableProperty]
    private string _cpuLoadLabel = "0%";

    [ObservableProperty]
    private string _memoryLoadLabel = "0%";

    [ObservableProperty]
    private string _diskLoadLabel = "0%";

    [ObservableProperty]
    private int _cpuLoadPercent;

    [ObservableProperty]
    private int _memoryLoadPercent;

    [ObservableProperty]
    private int _diskLoadPercent;

    private const double ResourceBarMaxWidth = 56;

    [ObservableProperty]
    private double _cpuLoadBarWidth;

    [ObservableProperty]
    private double _memoryLoadBarWidth;

    [ObservableProperty]
    private double _diskLoadBarWidth;

    public string CpuLoadLine => $"CPU {CpuLoadLabel}";

    public string MemoryLoadLine => $"Memory {MemoryLoadLabel}";

    public string DiskLoadLine => $"Disk {DiskLoadLabel}";

    [ObservableProperty]
    private string _frameDropsLabel = "0 (0.0%)";

    [ObservableProperty]
    private string _liveClockLabel = TransportFormatting.FormatElapsed(0);

    [ObservableProperty]
    private int _masterLevelLeft;

    [ObservableProperty]
    private int _masterLevelRight;

    [ObservableProperty]
    private string _masterVolumeLabel = "0.0 dB";

    [ObservableProperty]
    private string _masterLufsLabel = "LUFS —";

    [ObservableProperty]
    private string _masterMeterLabel = "Meter —";

    private Brush _programStatHealthBrush = TransportFormatting.HealthBrush(OutputHealthKind.Good);

    private Brush _streamStatHealthBrush = TransportFormatting.HealthBrush(OutputHealthKind.Idle);

    private Brush _recordStatHealthBrush = TransportFormatting.HealthBrush(OutputHealthKind.Idle);

    public Brush ProgramStatHealthBrush
    {
        get => _programStatHealthBrush;
        private set => SetProperty(ref _programStatHealthBrush, value);
    }

    public Brush StreamStatHealthBrush
    {
        get => _streamStatHealthBrush;
        private set => SetProperty(ref _streamStatHealthBrush, value);
    }

    public Brush RecordStatHealthBrush
    {
        get => _recordStatHealthBrush;
        private set => SetProperty(ref _recordStatHealthBrush, value);
    }

    public Brush SetAndForgetButtonBackground => SetAndForgetSelected
        ? new SolidColorBrush(Windows.UI.Color.FromArgb(31, 61, 220, 151))
        : new SolidColorBrush(Windows.UI.Color.FromArgb(0, 0, 0, 0));

    public Brush SetAndForgetButtonBorder => SetAndForgetSelected
        ? new SolidColorBrush(Windows.UI.Color.FromArgb(140, 61, 220, 151))
        : new SolidColorBrush(Windows.UI.Color.FromArgb(255, 42, 52, 60));

    public Brush SetAndForgetToggleBackground => SetAndForgetSelected
        ? new SolidColorBrush(Windows.UI.Color.FromArgb(77, 61, 220, 151))
        : new SolidColorBrush(Windows.UI.Color.FromArgb(31, 255, 255, 255));

    public Brush SetAndForgetThumbBrush => SetAndForgetSelected
        ? new SolidColorBrush(Windows.UI.Color.FromArgb(255, 61, 220, 151))
        : new SolidColorBrush(Windows.UI.Color.FromArgb(255, 148, 165, 155));

    public Thickness SetAndForgetThumbMargin => SetAndForgetSelected
        ? new Thickness(16, 2, 2, 2)
        : new Thickness(2);

    public double MasterLevelLeftBarWidth => MasterLevelLeft * 1.2;

    public double MasterLevelRightBarWidth => MasterLevelRight * 1.2;

    partial void OnSetAndForgetSelectedChanged(bool value)
    {
        OnPropertyChanged(nameof(SetAndForgetButtonBackground));
        OnPropertyChanged(nameof(SetAndForgetButtonBorder));
        OnPropertyChanged(nameof(SetAndForgetToggleBackground));
        OnPropertyChanged(nameof(SetAndForgetThumbBrush));
        OnPropertyChanged(nameof(SetAndForgetThumbMargin));
    }

    partial void OnMasterLevelLeftChanged(int value) =>
        OnPropertyChanged(nameof(MasterLevelLeftBarWidth));

    partial void OnMasterLevelRightChanged(int value) =>
        OnPropertyChanged(nameof(MasterLevelRightBarWidth));

    partial void OnCpuLoadLabelChanged(string value)
    {
        OnPropertyChanged(nameof(CpuLoadLine));
    }

    partial void OnMemoryLoadLabelChanged(string value)
    {
        OnPropertyChanged(nameof(MemoryLoadLine));
    }

    partial void OnDiskLoadLabelChanged(string value)
    {
        OnPropertyChanged(nameof(DiskLoadLine));
    }

    public void ApplySystemResourceSample(int cpuPercent, int memoryPercent, int diskPercent)
    {
        CpuLoadPercent = Math.Clamp(cpuPercent, 0, 100);
        MemoryLoadPercent = Math.Clamp(memoryPercent, 0, 100);
        DiskLoadPercent = Math.Clamp(diskPercent, 0, 100);
        CpuLoadLabel = TransportFormatting.PercentLabel(CpuLoadPercent);
        MemoryLoadLabel = TransportFormatting.PercentLabel(MemoryLoadPercent);
        DiskLoadLabel = TransportFormatting.PercentLabel(DiskLoadPercent);
        CpuLoadBarWidth = ResourceBarMaxWidth * CpuLoadPercent / 100d;
        MemoryLoadBarWidth = ResourceBarMaxWidth * MemoryLoadPercent / 100d;
        DiskLoadBarWidth = ResourceBarMaxWidth * DiskLoadPercent / 100d;
    }

    public void ApplyAutomationState(ProductionMode mode, bool canToggle)
    {
        SetAndForgetSelected = mode == ProductionMode.SetAndForget;
        SetAndForgetCanToggle = canToggle;
        AutomationStatusLabel = SetAndForgetSelected ? "On" : "Off";
    }

    public void ApplyMeetingState(bool inMeeting) => MagicSceneEnabled = inMeeting;

    public void ApplyIdleState(bool recording, bool streaming, string programResolutionLabel)
    {
        ProgramStatValue = string.IsNullOrWhiteSpace(programResolutionLabel) ? "—" : programResolutionLabel;
        ApplyHealth(ProgramStatHealthLabel, OutputHealthKind.Idle, brush => ProgramStatHealthBrush = brush);

        StreamStatValue = streaming ? programResolutionLabel : "Idle";
        ApplyHealth(
            StreamStatHealthLabel,
            streaming ? OutputHealthKind.Good : OutputHealthKind.Idle,
            brush => StreamStatHealthBrush = brush);

        RecordStatValue = recording ? programResolutionLabel : "Idle";
        ApplyHealth(
            RecordStatHealthLabel,
            recording ? OutputHealthKind.Good : OutputHealthKind.Idle,
            brush => RecordStatHealthBrush = brush);

        FrameDropsLabel = TransportFormatting.FrameDropsLabel(0, 0);
        LiveClockLabel = TransportFormatting.FormatElapsed(0);
        MasterLevelLeft = 0;
        MasterLevelRight = 0;
        MasterVolumeLabel = "—";
    }

    public void ApplySnapshot(
        NativeMediaCoreStateSnapshot snapshot,
        bool recording,
        bool streaming,
        string programResolutionLabel)
    {
        var elapsedSeconds = (int)Math.Floor(snapshot.Diagnostics.GeneratedAtMs / 1000d);
        var networkHealth = ResolveNetworkHealth(snapshot);
        var droppedFrames = ResolveDroppedFrames(snapshot);
        var totalFrames = Math.Max(snapshot.ProgramFrameCount, droppedFrames);
        var transportReadouts = LiveProductionSync.MapTransportReadouts(
            snapshot,
            recording,
            streaming,
            programResolutionLabel);

        ProgramStatValue = programResolutionLabel;
        ApplyHealth(ProgramStatHealthLabel, networkHealth, brush => ProgramStatHealthBrush = brush);

        StreamStatValue = transportReadouts.StreamStatValue;
        ApplyHealth(
            StreamStatHealthLabel,
            TransportFormatting.MapNetworkHealth(transportReadouts.StreamHealthStatus),
            brush => StreamStatHealthBrush = brush);

        RecordStatValue = transportReadouts.RecordStatValue;
        ApplyHealth(
            RecordStatHealthLabel,
            TransportFormatting.MapNetworkHealth(transportReadouts.RecordHealthStatus),
            brush => RecordStatHealthBrush = brush);

        FrameDropsLabel = TransportFormatting.FrameDropsLabel(droppedFrames, totalFrames);
        LiveClockLabel = TransportFormatting.FormatElapsed(elapsedSeconds);

        var masterLevel = snapshot.AudioMixSession.MasterLevel > 0
            ? snapshot.AudioMixSession.MasterLevel
            : 0;
        MasterLevelLeft = Math.Clamp(masterLevel, 0, 100);
        MasterLevelRight = MasterLevelLeft;
        MasterMeterLabel = $"Meter {MasterLevelLeft}%";
        MasterLufsLabel = snapshot.AudioMixSession.LoudnessLufs <= -59
            ? "LUFS —"
            : $"{snapshot.AudioMixSession.LoudnessLufs:0.0} LUFS";
        MasterVolumeLabel = snapshot.AudioMixSession.LimiterEnabled
            ? snapshot.AudioMixSession.LimiterActive ? "Limiter reducing" : "Limiter enabled"
            : "Limiter bypassed";
    }

    private static int EstimateEncoderLoad(
        NativeMediaCoreStateSnapshot snapshot,
        bool recording,
        bool streaming)
    {
        if (!recording && !streaming && snapshot.EncoderSession.Status is not "encoding")
        {
            return 0;
        }

        var load = 14 + snapshot.ProgramFrameCount % 18;
        if (streaming)
        {
            load += 8;
        }

        if (recording)
        {
            load += 6;
        }

        if (snapshot.Compositor.DroppedFrameCount > 0)
        {
            load += 4;
        }

        return Math.Min(100, load);
    }

    private static OutputHealthKind ResolveNetworkHealth(NativeMediaCoreStateSnapshot snapshot)
    {
        var health = snapshot.OutputHealth.FirstOrDefault(item =>
            item.Destination.Equals("rtmp", StringComparison.OrdinalIgnoreCase) ||
            item.Destination.Equals("program", StringComparison.OrdinalIgnoreCase));

        if (health is null)
        {
            health = snapshot.OutputHealth.FirstOrDefault();
        }

        return health is null
            ? OutputHealthKind.Idle
            : TransportFormatting.MapNetworkHealth(health.Status);
    }

    private static int ResolveDroppedFrames(NativeMediaCoreStateSnapshot snapshot)
    {
        var healthDrops = snapshot.OutputHealth.Sum(item => item.DroppedFrames);
        var compositorDrops = snapshot.Compositor.DroppedFrameCount;
        var recordingDrops = snapshot.Recording?.TotalDroppedFrames ?? 0;
        return Math.Max(healthDrops, Math.Max(compositorDrops, recordingDrops));
    }

    private void ApplyHealth(
        string propertyName,
        OutputHealthKind health,
        Action<Brush> setBrush)
    {
        setBrush(TransportFormatting.HealthBrush(health));
        switch (propertyName)
        {
            case nameof(ProgramStatHealthLabel):
                ProgramStatHealthLabel = TransportFormatting.HealthLabel(health);
                break;
            case nameof(StreamStatHealthLabel):
                StreamStatHealthLabel = TransportFormatting.HealthLabel(health);
                break;
            case nameof(RecordStatHealthLabel):
                RecordStatHealthLabel = TransportFormatting.HealthLabel(health);
                break;
        }
    }
}
