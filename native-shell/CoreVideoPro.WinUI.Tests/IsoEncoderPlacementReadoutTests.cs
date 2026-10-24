using CoreVideoPro.MediaCore.Models;
using CoreVideoPro.WinUI.ViewModels.Transport;
using Xunit;

namespace CoreVideoPro.WinUI.Tests;

/// <summary>
/// #935 / owner ruling D9: a software ISO is Informational with its measured per-frame cost;
/// Warning only when frames drop or the measured mean exceeds the measured frame budget.
/// Every number shown comes from the snapshot; absent means "not measured yet", never zero.
/// </summary>
public sealed class IsoEncoderPlacementReadoutTests
{
    [Fact]
    public void AllHardware_NoReadout()
    {
        var r = IsoEncoderPlacementReadout.Describe(Rec(Iso(1, "hardware")), true);
        Assert.Equal(PlacementSeverity.None, r.Severity);
        Assert.Empty(r.Lines);
    }

    [Fact]
    public void NoRecording_NoReadout()
        => Assert.Equal(PlacementSeverity.None, IsoEncoderPlacementReadout.Describe(null, false).Severity);

    [Fact]
    public void SoftwareIsoUnderBudget_IsInformational_WithMeasuredCost()
    {
        // 600 frames, 5,640,000 us of work => 9.4 ms per frame; 60 fps => 16.7 ms budget.
        var r = IsoEncoderPlacementReadout.Describe(Rec(Iso(8, "software", "Guest 8", work: 5_640_000, frames: 600, fps: 60)), true);
        Assert.Equal(PlacementSeverity.Informational, r.Severity);
        Assert.Equal("ISO-8 Guest 8 is on the CPU software encoder (GPU sessions used up) · 9.4 ms per frame of a 16.7 ms budget · 0 frames dropped", Assert.Single(r.Lines));
    }

    [Fact]
    public void SoftwareIsoOverBudget_IsWarning()
        => Assert.Equal(PlacementSeverity.Warning,
            IsoEncoderPlacementReadout.Describe(Rec(Iso(8, "software", work: 12_000_000, frames: 600, fps: 60)), true).Severity);

    [Fact]
    public void SoftwareIsoThatDropped_IsWarning()
        => Assert.Equal(PlacementSeverity.Warning,
            IsoEncoderPlacementReadout.Describe(Rec(Iso(8, "software", work: 1_000, frames: 600, fps: 60, dropped: 3)), true).Severity);

    [Fact]
    public void NoFramesYet_SaysNotMeasured_NeverZero()
    {
        var r = IsoEncoderPlacementReadout.Describe(Rec(Iso(8, "software", work: null, frames: 0, fps: 60)), true);
        var line = Assert.Single(r.Lines);
        Assert.Contains("cost not measured yet", line);
        Assert.DoesNotContain("0.0 ms", line);
        Assert.DoesNotContain("budget", line);
        Assert.Equal(PlacementSeverity.Informational, r.Severity);
    }

    [Fact]
    public void MissingFrameRate_OmitsBudget_DoesNotAssume60()
    {
        var r = IsoEncoderPlacementReadout.Describe(Rec(Iso(8, "software", work: 5_640_000, frames: 600, fps: null)), true);
        var line = Assert.Single(r.Lines);
        Assert.Contains("9.4 ms per frame", line);
        Assert.DoesNotContain("budget", line);
        Assert.Equal(PlacementSeverity.Informational, r.Severity);
    }

    [Fact]
    public void ProgramOnSoftware_GetsItsOwnLine()
    {
        var r = IsoEncoderPlacementReadout.Describe(Rec(), programHardwareAccelerated: false);
        Assert.Contains(r.Lines, l => l.StartsWith("Program is on the CPU software encoder", StringComparison.Ordinal));
        Assert.Equal(PlacementSeverity.Informational, r.Severity);
    }

    [Fact]
    public void ProgramStreamSaysHardware_NoProgramLineEvenIfFlagFalse()
        => Assert.Empty(IsoEncoderPlacementReadout.Describe(RecP(Program("hardware")), false).Lines);

    [Fact]
    public void ProgramOnSoftware_ShowsItsMeasuredCost()
    {
        var line = Assert.Single(IsoEncoderPlacementReadout.Describe(
            RecP(Program("software", work: 3_000_000, frames: 600, fps: 60)), false).Lines);
        Assert.Equal("Program is on the CPU software encoder · 5.0 ms per frame of a 16.7 ms budget · 0 frames dropped", line);
    }

    [Fact]
    public void MissingIsoIndex_FallsBackToDisplayName()
        => Assert.StartsWith("Guest 8 is on", Assert.Single(
            IsoEncoderPlacementReadout.Describe(Rec(Iso(null, "software", "Guest 8", work: 1, frames: 1, fps: 60)), true).Lines));

    [Fact]
    public void LinesAreInIsoIndexOrder()
    {
        var r = IsoEncoderPlacementReadout.Describe(Rec(
            Iso(9, "software", "B", work: 1, frames: 1, fps: 60), Iso(2, "software", "A", work: 1, frames: 1, fps: 60)), true);
        Assert.StartsWith("ISO-2 A", r.Lines[0]);
        Assert.StartsWith("ISO-9 B", r.Lines[1]);
    }

    [Theory]
    [InlineData("hardware-unavailable", "no GPU encoder")]
    [InlineData("operator-selected", "set to software")]
    [InlineData("something-new", "something-new")]
    public void ReasonMap(string reason, string expected)
        => Assert.Contains($"({expected})", Assert.Single(IsoEncoderPlacementReadout.Describe(
            Rec(Iso(3, "software", work: 1, frames: 1, fps: 60, reason: reason)), true).Lines));

    private static NativeMediaCoreRecordingStream Program(string? path, long? work = null, long? frames = null, double? fps = null)
        => new()
        {
            Kind = "program", Path = "Program.mp4", Status = "recording", EncoderPath = path,
            VideoWorkUs = work, MuxVideoFrameCount = frames, FrameRate = fps,
        };

    private static NativeMediaCoreRecordingStream Iso(
        int? index, string path, string name = "Guest", long? work = null, long? frames = null,
        double? fps = null, int dropped = 0, string reason = "hardware-capacity-exhausted")
        => new()
        {
            Kind = "iso", Path = $"ISO-{index}.mp4", Status = "recording", IsoIndex = index, DisplayName = name,
            SourceId = $"zoom:{index}", EncoderPath = path,
            FallbackReason = path == "software" ? reason : null,
            VideoWorkUs = work, MuxVideoFrameCount = frames, FrameRate = fps, DroppedFrames = dropped,
        };

    private static NativeMediaCoreRecordingSession Rec(params NativeMediaCoreRecordingStream[] isos)
        => RecP(Program(null), isos);

    private static NativeMediaCoreRecordingSession RecP(NativeMediaCoreRecordingStream program, params NativeMediaCoreRecordingStream[] isos)
        => new()
        {
            SessionId = "s", Active = true, Status = "recording", WriterStatus = "writing",
            TargetFolder = @"C:\r", FilenamePrefix = "show", Format = "mp4", Quality = "high", ProgramPath = @"C:\r\show.mp4",
            Streams = [program, .. isos],
        };
}
