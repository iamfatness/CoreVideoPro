using CoreVideoPro.MediaCore.Models;
using CoreVideoPro.MediaCore.Services;
using Xunit;

namespace CoreVideoPro.MediaCore.Tests;

public sealed class AudioValidationChecklistBuilderTests
{
    [Fact]
    public void Format_SummarizesPassingAudioChain()
    {
        var audio = new NativeMediaCoreAudioMixSession
        {
            Status = "live",
            Summary = "Program audio routed",
            MonitorEnabled = true,
            MonitorStatus = "playing",
            MonitorFramesPlayed = 480
        };
        var capture = RoutedCapture(SignaledSource());
        var senders = new NativeMediaCoreOutputSenderSession
        {
            Status = "live",
            ActiveSenderCount = 1,
            Senders =
            [
                new NativeMediaCoreOutputSender
                {
                    SenderId = "rtmp",
                    Destination = "rtmp",
                    Status = "live",
                    AudioFramesSent = 960
                }
            ]
        };
        var recording = ActiveRecording(new NativeMediaCoreRecordingProof
        {
            AudioPacketsObserved = 2,
            AudioPresent = true,
            AudioSampleCount = 960,
            AudioChannels = 2,
            AudioSampleRate = 48000
        });

        var status = AudioValidationChecklistBuilder.Format(audio, capture, senders, recording);
        var acceptance = AudioValidationChecklistBuilder.SummarizeAcceptance(audio, capture, senders, recording);

        Assert.Equal("CAPTURE ok 960f | PGM ok 960f | MON ok 480f | STREAM ok 960f | RECORD ok 960 samples", status);
        Assert.Equal("Audio validated: capture, PGM, MON, stream, recording all have current proof.", acceptance);
        Assert.Equal(
            "No blocking audio stage detected.",
            AudioValidationChecklistBuilder.FindFirstBlockingStage(audio, capture, senders, recording));
        Assert.Equal(
            "Full audio chain validated: capture, PGM, MON, stream, and recording all have current proof.",
            AudioValidationChecklistBuilder.SummarizeFullChain(audio, capture, senders, recording));
    }

    [Fact]
    public void Build_DoesNotPassCaptureFromSilentPcm()
    {
        var capture = RoutedCapture(new NativeMediaCoreCaptureAudioSource
        {
            CaptureDeviceId = "local-machine-audio",
            AudioDeviceName = "System loopback",
            CaptureStreaming = true,
            CaptureFramesReceived = 960,
            SignalPresent = false
        });

        var checklist = AudioValidationChecklistBuilder.Build(BasicAudio(), capture);

        Assert.Equal("CAPTURE silent 960f", checklist[0]);
        Assert.Equal("PGM silent source 960f", checklist[1]);
        Assert.Equal(
            "Audio validation incomplete: fix source audio first; play signal through the selected source or choose another input/loopback/process.",
            AudioValidationChecklistBuilder.SummarizeAcceptance(checklist));
    }

    [Fact]
    public void Build_DoesNotPassCaptureWhenLastPcmFrameIsStale()
    {
        var capture = RoutedCapture(new NativeMediaCoreCaptureAudioSource
        {
            CaptureDeviceId = "local-machine-audio",
            AudioDeviceName = "System loopback",
            CaptureStreaming = true,
            CaptureFramesReceived = 960,
            CaptureLastFrameAgeMs = 1500,
            SignalPresent = true
        });

        var checklist = AudioValidationChecklistBuilder.Build(BasicAudio(), capture);

        Assert.Equal("CAPTURE stale 960f age>1000ms", checklist[0]);
        Assert.Equal("PGM stale source 960f", checklist[1]);
        Assert.Equal("First audio block: CAPTURE stale 960f age>1000ms.", AudioValidationChecklistBuilder.FindFirstBlockingStage(checklist));
        Assert.Equal(
            "Audio validation incomplete: source audio is stale; restart or reselect the capture source.",
            AudioValidationChecklistBuilder.SummarizeAcceptance(checklist));
    }

    [Fact]
    public void Build_PassesCaptureWhenLastPcmFrameIsFresh()
    {
        var capture = RoutedCapture(new NativeMediaCoreCaptureAudioSource
        {
            CaptureDeviceId = "local-machine-audio",
            AudioDeviceName = "System loopback",
            CaptureStreaming = true,
            CaptureFramesReceived = 960,
            CaptureLastFrameAgeMs = 100,
            SignalPresent = true
        });

        var checklist = AudioValidationChecklistBuilder.Build(BasicAudio(), capture);

        Assert.Equal("CAPTURE ok 960f", checklist[0]);
    }

    [Fact]
    public void Build_FlagsMonitorOutputGap()
    {
        var audio = new NativeMediaCoreAudioMixSession
        {
            Status = "live",
            Summary = "Program audio routed",
            MonitorEnabled = true,
            MonitorStatus = "armed",
            MonitorFramesPlayed = 0
        };

        var checklist = AudioValidationChecklistBuilder.Build(audio, RoutedCapture(SignaledSource()));

        Assert.Equal("MON wait output 480f", checklist[2]);
        Assert.Equal("First audio block: MON wait output 480f.", AudioValidationChecklistBuilder.FindFirstBlockingStage(checklist));
        Assert.Equal(
            "Full audio chain incomplete: MON, stream, recording not yet proven.",
            AudioValidationChecklistBuilder.SummarizeFullChain(checklist));
        Assert.Equal(
            "Audio validation incomplete: MON has PCM; check monitor output device and volume.",
            AudioValidationChecklistBuilder.SummarizeAcceptance(checklist));
    }

    [Fact]
    public void Build_FlagsActiveRecordingWithoutProofTelemetry()
    {
        var recording = ActiveRecording(proof: null);

        var checklist = AudioValidationChecklistBuilder.Build(
            BasicAudio(),
            RoutedCapture(SignaledSource()),
            recording: recording);

        Assert.Equal("RECORD wait proof", checklist[4]);
        Assert.Equal(
            "Audio validation incomplete: active recording has not reported native audio proof telemetry yet.",
            AudioValidationChecklistBuilder.SummarizeAcceptance(checklist));
    }

    [Fact]
    public void Build_DoesNotPassStreamFromSenderFramesWhenCaptureIsNotVerified()
    {
        var capture = new NativeMediaCoreCaptureAudioSources
        {
            Status = "ready",
            Summary = "Capture audio waiting for PCM",
            SourceCount = 1,
            StreamingCount = 1,
            CaptureFramesReceived = 0,
            RoutedMasterFrames = 0,
            RoutedStreamFrames = 0
        };
        var senders = new NativeMediaCoreOutputSenderSession
        {
            Status = "live",
            ActiveSenderCount = 1,
            Senders =
            [
                new NativeMediaCoreOutputSender
                {
                    SenderId = "rtmp",
                    Destination = "rtmp",
                    Status = "live",
                    AudioFramesSent = 960
                }
            ]
        };

        var checklist = AudioValidationChecklistBuilder.Build(BasicAudio(), capture, senders);

        Assert.Equal("CAPTURE wait no PCM", checklist[0]);
        Assert.Equal("STREAM wait source 960f", checklist[3]);
        Assert.Equal(
            "Audio validation incomplete: add or enable a local/capture audio source, then play signal through it.",
            AudioValidationChecklistBuilder.SummarizeAcceptance(checklist));
    }

    [Fact]
    public void Build_FlagsStreamRouteBeforeSenderWhenStreamBusHasNoPcm()
    {
        var capture = RoutedCapture(SignaledSource(), routedStreamFrames: 0);
        var senders = ActiveSender(audioFramesSent: 0);

        var checklist = AudioValidationChecklistBuilder.Build(BasicAudio(), capture, senders);

        Assert.Equal("STREAM wait route", checklist[3]);
        Assert.Equal("First audio block: STREAM wait route.", AudioValidationChecklistBuilder.FindFirstBlockingStage(checklist));
        Assert.Equal(
            "Audio validation incomplete: route verified source audio to stream or master before streaming.",
            AudioValidationChecklistBuilder.SummarizeAcceptance(checklist));
    }

    [Fact]
    public void Build_FlagsStreamSenderAfterStreamBusHasPcm()
    {
        var capture = RoutedCapture(SignaledSource(), routedStreamFrames: 960);
        var senders = ActiveSender(audioFramesSent: 0);

        var checklist = AudioValidationChecklistBuilder.Build(BasicAudio(), capture, senders);

        Assert.Equal("STREAM wait sender", checklist[3]);
        Assert.Equal("First audio block: STREAM wait sender.", AudioValidationChecklistBuilder.FindFirstBlockingStage(checklist));
        Assert.Equal(
            "Audio validation incomplete: stream has bus audio but the sender has not accepted audio frames yet.",
            AudioValidationChecklistBuilder.SummarizeAcceptance(checklist));
    }

    [Fact]
    public void Build_PassesRecordingProofWhenOptionalCaptureIsNotVerified()
    {
        var capture = new NativeMediaCoreCaptureAudioSources
        {
            Status = "ready",
            Summary = "Capture audio waiting for PCM",
            SourceCount = 1,
            StreamingCount = 1,
            CaptureFramesReceived = 0,
            RoutedMasterFrames = 0
        };
        var recording = ActiveRecording(new NativeMediaCoreRecordingProof
        {
            AudioPacketsObserved = 2,
            AudioPresent = true,
            AudioSampleCount = 960,
            AudioChannels = 2,
            AudioSampleRate = 48000
        });

        var checklist = AudioValidationChecklistBuilder.Build(BasicAudio(), capture, recording: recording);

        Assert.Equal("CAPTURE wait no PCM", checklist[0]);
        Assert.Equal("RECORD ok 960 samples", checklist[4]);
        Assert.Equal(
            "Audio validation incomplete: add or enable a local/capture audio source, then play signal through it.",
            AudioValidationChecklistBuilder.SummarizeAcceptance(checklist));
    }

    [Fact]
    public void Build_ValidatesZoomProgramAndRecordingWhileOptionalLocalCaptureIsSilent()
    {
        var audio = new NativeMediaCoreAudioMixSession
        {
            Status = "live",
            Summary = "Zoom program audio routed",
            MasterLevel = 54,
            MixedFrameCount = 480,
            Participants =
            [
                new NativeMediaCoreParticipantAudioChannel
                {
                    ParticipantId = "zoom-guest",
                    InputLevel = 51,
                    OutputLevel = 59,
                    PeakDbfs = -9.5,
                    Status = "balanced"
                }
            ]
        };
        var capture = RoutedCapture(new NativeMediaCoreCaptureAudioSource
        {
            CaptureDeviceId = "local-mic",
            AudioDeviceName = "Unused local microphone",
            CaptureStreaming = true,
            CaptureFramesReceived = 960,
            SignalPresent = false
        });
        var recording = ActiveRecording(new NativeMediaCoreRecordingProof
        {
            AudioPacketsObserved = 240,
            AudioPresent = true,
            AudioSampleCount = 230400,
            AudioChannels = 2,
            AudioSampleRate = 48000
        });

        var checklist = AudioValidationChecklistBuilder.Build(audio, capture, recording: recording);

        Assert.Equal("CAPTURE silent 960f", checklist[0]);
        Assert.Equal("PGM ok 480f", checklist[1]);
        Assert.Equal("RECORD ok 230400 samples", checklist[4]);
        Assert.Equal("No blocking audio stage detected.", AudioValidationChecklistBuilder.FindFirstBlockingStage(checklist));
        Assert.Equal(
            "Audio validated: PGM, recording. monitor, stream idle. Local capture is optional and remains unvalidated (CAPTURE silent 960f).",
            AudioValidationChecklistBuilder.SummarizeAcceptance(checklist));
    }

    [Fact]
    public void Build_FlagsRecordingRouteBeforeWriterWhenMasterBusHasNoPcm()
    {
        var capture = RoutedCapture(SignaledSource(), routedMasterFrames: 0);
        var recording = ActiveRecording(proof: null);

        var checklist = AudioValidationChecklistBuilder.Build(BasicAudio(), capture, recording: recording);

        Assert.Equal("RECORD wait route", checklist[4]);
        Assert.Equal("First audio block: PGM wait route.", AudioValidationChecklistBuilder.FindFirstBlockingStage(checklist));
        Assert.Equal(
            "Audio validation incomplete: route source audio to master/PGM.",
            AudioValidationChecklistBuilder.SummarizeAcceptance(checklist));
    }

    [Fact]
    public void Build_FlagsRecordingWriterAfterMasterBusHasPcm()
    {
        var capture = RoutedCapture(SignaledSource(), routedMasterFrames: 960);
        var recording = ActiveRecording(new NativeMediaCoreRecordingProof
        {
            AudioPacketsObserved = 0,
            AudioPresent = false,
            AudioSampleCount = 0,
            AudioChannels = 2,
            AudioSampleRate = 48000
        });

        var checklist = AudioValidationChecklistBuilder.Build(BasicAudio(), capture, recording: recording);

        Assert.Equal("RECORD wait writer", checklist[4]);
        Assert.Equal("First audio block: RECORD wait writer.", AudioValidationChecklistBuilder.FindFirstBlockingStage(checklist));
        Assert.Equal(
            "Audio validation incomplete: master bus has PCM but the recording writer has not accepted audio yet.",
            AudioValidationChecklistBuilder.SummarizeAcceptance(checklist));
    }

    private static NativeMediaCoreCaptureAudioSources RoutedCapture(
        NativeMediaCoreCaptureAudioSource source,
        int routedMasterFrames = 960,
        int routedStreamFrames = 960) =>
        new()
        {
            Status = "ready",
            Summary = "Capture audio routed",
            SourceCount = 1,
            StreamingCount = source.CaptureStreaming ? 1 : 0,
            CaptureFramesReceived = source.CaptureFramesReceived,
            RoutedMasterFrames = routedMasterFrames,
            RoutedStreamFrames = routedStreamFrames,
            RoutedMonitorFrames = 480,
            Sources = [source]
        };

    private static NativeMediaCoreCaptureAudioSource SignaledSource() =>
        new()
        {
            CaptureDeviceId = "local-machine-audio",
            AudioDeviceName = "System loopback",
            CaptureStreaming = true,
            CaptureFramesReceived = 960,
            SignalPresent = true
        };

    private static NativeMediaCoreRecordingSession ActiveRecording(NativeMediaCoreRecordingProof? proof) =>
        new()
        {
            SessionId = "rec-1",
            Active = true,
            Status = "recording",
            WriterStatus = "recording",
            TargetFolder = "recordings",
            FilenamePrefix = "program",
            Format = "mp4",
            Quality = "high",
            ProgramPath = "recordings/program.mp4",
            Proof = proof
        };

    private static NativeMediaCoreOutputSenderSession ActiveSender(long audioFramesSent) =>
        new()
        {
            Status = "live",
            ActiveSenderCount = 1,
            Senders =
            [
                new NativeMediaCoreOutputSender
                {
                    SenderId = "rtmp",
                    Destination = "rtmp",
                    Status = "live",
                    AudioFramesSent = audioFramesSent
                }
            ]
        };

    private static NativeMediaCoreAudioMixSession BasicAudio() =>
        new()
        {
            Status = "live",
            Summary = "Program audio routed"
        };
}
