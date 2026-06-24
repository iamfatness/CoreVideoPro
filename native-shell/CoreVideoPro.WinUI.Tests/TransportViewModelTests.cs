using CoreVideoPro.MediaCore.Models;
using CoreVideoPro.WinUI.ViewModels;
using Xunit;

namespace CoreVideoPro.WinUI.Tests;

public sealed class TransportViewModelTests
{
    [Fact]
    public void ApplySnapshot_UsesMeasuredProgramBusPeaksForMasterMeters()
    {
        var snapshot = new NativeMediaCoreStateSnapshot
        {
            AudioMixSession = new NativeMediaCoreAudioMixSession
            {
                Status = "live",
                Summary = "Program mix balanced",
                MasterLevel = 88,
                LoudnessLufs = -18,
                MixedFrameCount = 12
            },
            AudioRoutingMatrix = new NativeMediaCoreAudioRoutingMatrix
            {
                Status = "live",
                Summary = "Routed",
                ProgramTapFrames = 480,
                BusTaps =
                [
                    new NativeMediaCoreAudioBusTap { BusId = "pgm-l", Frames = 480, PeakDbfs = -80, RmsDbfs = -86 },
                    new NativeMediaCoreAudioBusTap { BusId = "pgm-r", Frames = 480, PeakDbfs = -20, RmsDbfs = -26 }
                ]
            }
        };

        var readout = TransportViewModel.ResolveMasterMeterReadout(snapshot);

        Assert.Equal(0, readout.LeftLevel);
        Assert.Equal(67, readout.RightLevel);
        Assert.Equal("Peak -20.0 dBFS", readout.PeakLabel);
    }

    [Fact]
    public void ApplySnapshot_DoesNotShowSyntheticMasterWhenProgramTapIsSilent()
    {
        var snapshot = new NativeMediaCoreStateSnapshot
        {
            AudioMixSession = new NativeMediaCoreAudioMixSession
            {
                Status = "live",
                Summary = "Program mix balanced",
                MasterLevel = 76,
                LoudnessLufs = -60,
                MixedFrameCount = 20
            },
            AudioRoutingMatrix = new NativeMediaCoreAudioRoutingMatrix
            {
                Status = "live",
                Summary = "Routed",
                ProgramTapFrames = 480
            }
        };

        var readout = TransportViewModel.ResolveMasterMeterReadout(snapshot);

        Assert.Equal(0, readout.LeftLevel);
        Assert.Equal(0, readout.RightLevel);
    }

    [Fact]
    public void ApplySnapshot_DoesNotShowSyntheticMasterWhenNoProgramPcmExists()
    {
        var snapshot = new NativeMediaCoreStateSnapshot
        {
            AudioMixSession = new NativeMediaCoreAudioMixSession
            {
                Status = "live",
                Summary = "Metadata-only mix",
                MasterLevel = 68,
                LoudnessLufs = -60,
                MixedFrameCount = 0
            },
            AudioRoutingMatrix = new NativeMediaCoreAudioRoutingMatrix
            {
                Status = "live",
                Summary = "Routed",
                ProgramTapFrames = 0,
                BusTaps = []
            }
        };

        var readout = TransportViewModel.ResolveMasterMeterReadout(snapshot);

        Assert.Equal(0, readout.LeftLevel);
        Assert.Equal(0, readout.RightLevel);
        Assert.Null(readout.PeakLabel);
    }
}
