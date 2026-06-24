using CoreVideoPro.MediaCore.Models;
using CoreVideoPro.MediaCore.Services;
using Xunit;

namespace CoreVideoPro.MediaCore.Tests;

public sealed class TransportMappingTests
{
    [Fact]
    public void SummarizeRecordStatUsesProgramPathWhenRecordingIsActive()
    {
        var snapshot = BuildSnapshot(
            recordingActive: true,
            programPath: "Recordings/program-program-0.mp4");

        var label = LiveProductionSync.SummarizeRecordStat(snapshot, recording: true);

        Assert.Equal("Recordings/program-program-0.mp4", label);
    }

    [Fact]
    public void SummarizeRecordStatUsesRecordingArtifactPathFromWireWarnings()
    {
        var snapshot = BuildSnapshot() with
        {
            Warnings = ["Recording artifact: C:/Temp/corevideo-mf-recording-123.mp4"]
        };

        var label = LiveProductionSync.SummarizeRecordStat(snapshot, recording: true);

        Assert.Equal("C:/Temp/corevideo-mf-recording-123.mp4", label);
    }

    [Fact]
    public void SummarizeStreamStatReportsLiveRtmpDestinationStatusAndBitrate()
    {
        var snapshot = BuildSnapshot(streamingLive: true);

        var label = LiveProductionSync.SummarizeStreamStat(snapshot, streaming: true, programResolutionLabel: "1080p60");

        Assert.Equal("RTMP · live · 6 Mbps", label);
    }

    [Fact]
    public void SummarizeStreamStatReturnsIdleWhenNoLiveOutputs()
    {
        var snapshot = BuildSnapshot();

        var label = LiveProductionSync.SummarizeStreamStat(snapshot, streaming: false, programResolutionLabel: "1080p60");

        Assert.Equal("Idle", label);
    }

    [Fact]
    public void MapTransportReadoutsMapsIdleSnapshotToIdleTransportLabels()
    {
        var snapshot = BuildSnapshot();

        var readouts = LiveProductionSync.MapTransportReadouts(
            snapshot,
            recording: false,
            streaming: false,
            programResolutionLabel: "1080p60");

        Assert.Equal("Idle", readouts.RecordStatValue);
        Assert.Equal("Idle", readouts.StreamStatValue);
        Assert.Equal("idle", readouts.RecordHealthStatus);
        Assert.Equal("idle", readouts.StreamHealthStatus);
    }

    [Fact]
    public void MapTransportReadoutsMapsRecordingAndStreamingSnapshot()
    {
        var snapshot = BuildSnapshot(
            recordingActive: true,
            streamingLive: true,
            programPath: "Recordings/program-program-0.mp4");

        var readouts = LiveProductionSync.MapTransportReadouts(
            snapshot,
            recording: true,
            streaming: true,
            programResolutionLabel: "1080p60");

        Assert.Equal("Recordings/program-program-0.mp4", readouts.RecordStatValue);
        Assert.Equal("RTMP · live · 6 Mbps", readouts.StreamStatValue);
        Assert.Equal("live", readouts.RecordHealthStatus);
        Assert.Equal("live", readouts.StreamHealthStatus);
    }

    [Fact]
    public void SummarizeLocalOutputsAvoidsDemoFilenamesAndDestinations()
    {
        Assert.Equal("Outputs idle", LiveProductionSync.SummarizeLocalOutputs(recording: false, streaming: false));
        Assert.Equal("Recording program", LiveProductionSync.SummarizeLocalOutputs(recording: true, streaming: false));
        Assert.Equal("Streaming live", LiveProductionSync.SummarizeLocalOutputs(recording: false, streaming: true));
        Assert.Equal("Recording and streaming", LiveProductionSync.SummarizeLocalOutputs(recording: true, streaming: true));
    }

    [Fact]
    public void SummarizeOutputsUsesProgramPathInsteadOfDemoStrings()
    {
        var snapshot = BuildSnapshot(
            recordingActive: true,
            programPath: "Recordings/program-program-0.mp4");

        var status = MediaCoreBridgeService.SummarizeOutputs(snapshot);

        Assert.Equal("Recording Recordings/program-program-0.mp4", status);
    }

    [Fact]
    public void SummarizeOutputsReportsSenderWarningWhenNoOutputIsLive()
    {
        var snapshot = BuildSnapshot() with
        {
            OutputSenderSession = new NativeMediaCoreOutputSenderSession
            {
                Status = "warning",
                ActiveSenderCount = 0,
                Warnings = ["RTMP sender needs a configured RTMP/RTMPS server URL and stream key."]
            }
        };

        var status = MediaCoreBridgeService.SummarizeOutputs(snapshot);

        Assert.Equal("Output warning: RTMP sender needs a configured RTMP/RTMPS server URL and stream key.", status);
    }

    [Fact]
    public void SummarizeOutputsReportsFailedOutputHealthBeforeIdle()
    {
        var snapshot = BuildSnapshot() with
        {
            OutputHealth =
            [
                new NativeMediaCoreOutputHealth
                {
                    Destination = "rtmp",
                    Status = "failed",
                    Message = "FFmpeg exited with code 1 after ingest rejected credentials.",
                    DroppedFrames = 0
                }
            ]
        };

        var status = MediaCoreBridgeService.SummarizeOutputs(snapshot);

        Assert.Equal("RTMP output failed: FFmpeg exited with code 1 after ingest rejected credentials.", status);
    }

    [Fact]
    public void SummarizeOutputsDoesNotReportWarningOutputAsLive()
    {
        var snapshot = BuildSnapshot() with
        {
            OutputHealth =
            [
                new NativeMediaCoreOutputHealth
                {
                    Destination = "rtmp",
                    Status = "warning",
                    Message = "RTMP sender is retrying after connection refused.",
                    DroppedFrames = 0
                }
            ]
        };

        var status = MediaCoreBridgeService.SummarizeOutputs(snapshot);

        Assert.Equal("RTMP output warning: RTMP sender is retrying after connection refused.", status);
    }

    [Fact]
    public void SummarizeOutputsReportsFailedSenderWarningWhenSessionWarningsAreEmpty()
    {
        var snapshot = BuildSnapshot() with
        {
            OutputSenderSession = new NativeMediaCoreOutputSenderSession
            {
                Status = "failed",
                ActiveSenderCount = 0,
                Senders =
                [
                    new NativeMediaCoreOutputSender
                    {
                        SenderId = "rtmp:program",
                        Destination = "rtmp",
                        Status = "failed",
                        FramesSent = 0,
                        RetryCount = 0,
                        LatencyMs = 0,
                        BitrateMbps = 6,
                        Warning = "FFmpeg stdin write failed; the RTMP process stopped or rejected frames."
                    }
                ]
            }
        };

        var status = MediaCoreBridgeService.SummarizeOutputs(snapshot);

        Assert.Equal("RTMP output failed: FFmpeg stdin write failed; the RTMP process stopped or rejected frames.", status);
    }

    private static NativeMediaCoreStateSnapshot BuildSnapshot(
        bool recordingActive = false,
        bool streamingLive = false,
        string programPath = "Recordings/program-program-0.mp4")
    {
        NativeMediaCoreRecordingSession? recording = recordingActive
            ? new NativeMediaCoreRecordingSession
            {
                SessionId = "show-1",
                Active = true,
                Status = "recording",
                WriterStatus = "writing",
                StartedAtMs = 1000,
                ElapsedMs = 2000,
                TargetFolder = "Recordings",
                FilenamePrefix = "program",
                Format = "mp4",
                Quality = "high",
                ProgramPath = programPath
            }
            : null;

        var outputHealth = new List<NativeMediaCoreOutputHealth>();
        if (recordingActive)
        {
            outputHealth.Add(new NativeMediaCoreOutputHealth
            {
                Destination = "recording",
                Status = "live",
                Message = programPath,
                DroppedFrames = 0
            });
        }

        if (streamingLive)
        {
            outputHealth.Add(new NativeMediaCoreOutputHealth
            {
                Destination = "rtmp",
                Status = "live",
                Message = "RTMP sender live.",
                DroppedFrames = 0
            });
        }

        var outputSenderSession = streamingLive
            ? new NativeMediaCoreOutputSenderSession
            {
                Status = "live",
                ActiveSenderCount = 1,
                Senders =
                [
                    new NativeMediaCoreOutputSender
                    {
                        SenderId = "rtmp:program",
                        Destination = "rtmp",
                        Status = "live",
                        FramesSent = 12,
                        RetryCount = 0,
                        LatencyMs = 2100,
                        BitrateMbps = 6
                    }
                ]
            }
            : new NativeMediaCoreOutputSenderSession { Status = "idle" };

        return new NativeMediaCoreStateSnapshot
        {
            Recording = recording,
            OutputHealth = outputHealth,
            OutputSenderSession = outputSenderSession,
            OutputProfile = new NativeMediaCoreOutputProfile
            {
                ProfileId = "1080p60",
                Resolution = "1920x1080",
                Width = 1920,
                Height = 1080,
                Fps = 60,
                TargetBitrateMbps = 6
            },
            EncoderSession = new NativeMediaCoreEncoderSession
            {
                Status = streamingLive ? "encoding" : "idle",
                Lifecycle = new NativeMediaCoreEncoderLifecycle
                {
                    Status = streamingLive ? "encoding" : "idle",
                    LastTransition = "test"
                }
            }
        };
    }
}
