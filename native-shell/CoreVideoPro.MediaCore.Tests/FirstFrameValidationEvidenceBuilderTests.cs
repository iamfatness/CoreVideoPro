using CoreVideoPro.MediaCore.Models;
using CoreVideoPro.MediaCore.Services;
using Xunit;

namespace CoreVideoPro.MediaCore.Tests;

public sealed class FirstFrameValidationEvidenceBuilderTests
{
    [Fact]
    public void BuildsCompleteEvidenceFromSyntheticEventsAndSpineSnapshot()
    {
        var zoomPixels = new byte[8];
        var previewPixels = new byte[4];
        var spine = new ZoomMediaSpineNativeSnapshot
        {
            MeetingState = "in-meeting",
            ParticipantCount = 1,
            Participants =
            [
                new ZoomMediaSpineParticipant
                {
                    SdkUserId = "p1",
                    DisplayName = "Host",
                    AudioLevel = 71,
                    Talking = true
                }
            ],
            Subscriptions =
            [
                new ZoomMediaSpineSubscription
                {
                    ParticipantId = "p1",
                    Kind = "participant-video",
                    FramesReceived = 3,
                    AudioPacketsReceived = 8
                }
            ]
        };

        var evidence = FirstFrameValidationEvidenceBuilder.From(
            spineSnapshot: spine,
            zoomVideoFrame: new ZoomVideoFrame
            {
                ParticipantId = "p1",
                Width = 2,
                Height = 1,
                FrameId = 3,
                Bgra = zoomPixels
            },
            programPreviewFrame: new ProgramFramePreview
            {
                FrameNumber = 4,
                Width = 1,
                Height = 1,
                Bgra = previewPixels
            });

        Assert.True(evidence.Ready);
        Assert.True(evidence.ZoomVideoFrameObserved);
        Assert.True(evidence.ProgramPreviewFrameObserved);
        Assert.True(evidence.ParticipantFreshnessObserved);
        Assert.True(evidence.AudioFreshnessObserved);
        Assert.Empty(evidence.Missing);
        Assert.Equal("in_meeting", evidence.MeetingState);
        Assert.Equal("p1", evidence.LastZoomParticipantId);
        Assert.Equal(4, evidence.LastProgramFrameNumber);
    }

    [Fact]
    public void ReportsMissingSignalsWhenOnlyRosterIsFresh()
    {
        var evidence = FirstFrameValidationEvidenceBuilder.From(
            spineSnapshot: new ZoomMediaSpineNativeSnapshot
            {
                MeetingState = "in-meeting",
                ParticipantCount = 1,
                Participants =
                [
                    new ZoomMediaSpineParticipant
                    {
                        SdkUserId = "p1",
                        DisplayName = "Host",
                        Muted = true,
                        AudioLevel = 0,
                        Talking = false
                    }
                ]
            });

        Assert.False(evidence.Ready);
        Assert.True(evidence.ParticipantFreshnessObserved);
        Assert.False(evidence.ZoomVideoFrameObserved);
        Assert.False(evidence.ProgramPreviewFrameObserved);
        Assert.False(evidence.AudioFreshnessObserved);
        Assert.Contains("first Zoom video frame", evidence.Missing);
        Assert.Contains("first program preview frame", evidence.Missing);
        Assert.Contains("fresh participant audio", evidence.Missing);
    }

    [Fact]
    public void DerivesEvidenceFromNativeSnapshotWithoutLiveZoom()
    {
        var snapshot = SyntheticMediaCore.SynthesizeSnapshot([], 1500, 5) with
        {
            MeetingState = "in_meeting",
            Participants =
            [
                new RawParticipantEvent
                {
                    UserId = "p1",
                    DisplayName = "Host",
                    AudioLevel = 64
                }
            ],
            SourceSnapshot = new NativeMediaCoreFrameSourceSnapshot
            {
                AdapterId = "zoom-engine",
                Kind = "zoom-sdk",
                Status = "subscribed",
                SubscribedSourceCount = 1,
                LiveFrameCount = 2
            },
            ProgramFramePreview = new NativeMediaCoreProgramFramePreview
            {
                FrameNumber = 5,
                Width = 1,
                Height = 1,
                Bgra = new byte[4]
            },
            ProgramFrameCount = 5,
            AudioMixSession = new NativeMediaCoreAudioMixSession
            {
                Status = "live",
                MasterLevel = 70,
                LoudnessLufs = -16,
                MixedFrameCount = 9,
                Participants =
                [
                    new NativeMediaCoreParticipantAudioChannel
                    {
                        ParticipantId = "p1",
                        InputLevel = 55,
                        OutputLevel = 57,
                        GainDb = 0,
                        NoiseSuppression = true,
                        LimiterActive = false,
                        Muted = false,
                        Status = "balanced"
                    }
                ],
                Summary = "Mix live"
            }
        };

        var evidence = FirstFrameValidationEvidenceBuilder.From(snapshot: snapshot);

        Assert.True(evidence.Ready);
        Assert.Equal(2, evidence.LiveZoomFrameCount);
        Assert.Equal(5, evidence.ProgramFrameCount);
        Assert.Equal(9, evidence.AudioPacketCount);
        Assert.Equal(1, evidence.AudioActiveParticipantCount);
    }

    [Fact]
    public void AccumulatesEvidenceAcrossIndependentEvents()
    {
        var evidence = FirstFrameValidationEvidenceBuilder.From(
            zoomVideoFrame: new ZoomVideoFrame
            {
                ParticipantId = "p1",
                Width = 1,
                Height = 1,
                FrameId = 1,
                Bgra = new byte[4]
            });

        evidence = FirstFrameValidationEvidenceBuilder.From(
            existing: evidence,
            programPreviewFrame: new ProgramFramePreview
            {
                FrameNumber = 2,
                Width = 1,
                Height = 1,
                Bgra = new byte[4]
            },
            spineSnapshot: new ZoomMediaSpineNativeSnapshot
            {
                MeetingState = "in-meeting",
                ParticipantCount = 1,
                Subscriptions =
                [
                    new ZoomMediaSpineSubscription
                    {
                        ParticipantId = "p1",
                        Kind = "participant-audio",
                        AudioPacketsReceived = 4
                    }
                ]
            });

        Assert.True(evidence.Ready);
        Assert.Equal(1, evidence.LastZoomFrameId);
        Assert.Equal(2, evidence.LastProgramFrameNumber);
        Assert.Equal(4, evidence.AudioPacketCount);
    }

    [Fact]
    public void RecordsFirstZoomFrameTimingFromObservedElapsedMs()
    {
        var evidence = FirstFrameValidationEvidenceBuilder.From(
            zoomVideoFrame: new ZoomVideoFrame
            {
                ParticipantId = "42",
                Width = 2,
                Height = 1,
                FrameId = 1,
                Bgra = new byte[8]
            },
            zoomFrameObservedElapsedMs: 812.5);

        Assert.True(evidence.ZoomVideoFrameObserved);
        Assert.Equal(812.5, evidence.FirstZoomFrameElapsedMs);
        Assert.Equal(2, evidence.LastZoomFrameWidth);
        Assert.Equal(1, evidence.LastZoomFrameHeight);
        Assert.Equal("42", evidence.LastZoomParticipantId);
    }

    [Fact]
    public void PreservesEarliestFirstFrameTimingAcrossEvents()
    {
        var evidence = FirstFrameValidationEvidenceBuilder.From(
            zoomVideoFrame: new ZoomVideoFrame
            {
                ParticipantId = "42",
                Width = 1,
                Height = 1,
                FrameId = 1,
                Bgra = new byte[4]
            },
            zoomFrameObservedElapsedMs: 420);

        evidence = FirstFrameValidationEvidenceBuilder.From(
            existing: evidence,
            zoomVideoFrame: new ZoomVideoFrame
            {
                ParticipantId = "42",
                Width = 1,
                Height = 1,
                FrameId = 2,
                Bgra = new byte[4]
            },
            zoomFrameObservedElapsedMs: 900);

        Assert.Equal(420, evidence.FirstZoomFrameElapsedMs);
        Assert.Equal(2, evidence.LastZoomFrameId);
    }

    [Fact]
    public void DerivesFirstFrameTimingFromSpineSubscriptionStats()
    {
        var evidence = FirstFrameValidationEvidenceBuilder.From(
            spineSnapshot: new ZoomMediaSpineNativeSnapshot
            {
                MeetingState = "in-meeting",
                ParticipantCount = 1,
                Subscriptions =
                [
                    new ZoomMediaSpineSubscription
                    {
                        ParticipantId = "42",
                        Kind = "participant-video",
                        FramesReceived = 2,
                        FirstFrameAtMs = 256.5,
                        FirstFrameDelayMs = 256.5
                    }
                ]
            });

        Assert.True(evidence.ZoomVideoFrameObserved);
        Assert.Equal(256.5, evidence.FirstZoomFrameElapsedMs);
    }
}
