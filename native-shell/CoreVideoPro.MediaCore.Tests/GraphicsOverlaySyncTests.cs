using CoreVideoPro.MediaCore.Models;
using CoreVideoPro.MediaCore.Services;
using Xunit;

namespace CoreVideoPro.MediaCore.Tests;

public sealed class GraphicsOverlaySyncTests
{
    [Fact]
    public void AppendCaptionTranscriptFromSnapshotAddsCueAndDedupesRepeats()
    {
        var snapshot = BuildCaptionSnapshot("Welcome to the show.", "Sophia Martinez", atMs: 1200, confidence: 94);
        var speakerRoles = new Dictionary<string, string>(StringComparer.Ordinal)
        {
            ["Sophia Martinez"] = "Host"
        };

        var first = GraphicsOverlaySync.AppendCaptionTranscriptFromSnapshot(snapshot, [], speakerRoles);
        var second = GraphicsOverlaySync.AppendCaptionTranscriptFromSnapshot(snapshot, first, speakerRoles);

        Assert.Single(second);
        Assert.Equal("Sophia Martinez", second[0].SpeakerName);
        Assert.Equal("Host", second[0].Role);
        Assert.Equal("Welcome to the show.", second[0].Text);
        Assert.Equal(94, second[0].Confidence);
        Assert.Equal(first, second);
    }

    [Fact]
    public void AppendCaptionTranscriptFromSnapshotCapsRollingHistory()
    {
        IReadOnlyList<GraphicsOverlaySync.CaptionTranscriptEntryPatch> transcript = [];
        for (var index = 0; index < GraphicsOverlaySync.MaxTranscriptEntries + 2; index++)
        {
            transcript = GraphicsOverlaySync.AppendCaptionTranscriptFromSnapshot(
                BuildCaptionSnapshot($"Line {index}", "Speaker", index * 1000, 90),
                transcript);
        }

        Assert.Equal(GraphicsOverlaySync.MaxTranscriptEntries, transcript.Count);
        Assert.Equal("Line 2", transcript[0].Text);
        Assert.Equal("Line 7", transcript[^1].Text);
    }

    private static NativeMediaCoreStateSnapshot BuildSnapshot(
        IReadOnlyList<NativeMediaCoreRenderPlanLayer> layers) =>
        new()
        {
            OverlayCount = layers.Count(layer => layer.Kind.Equals("overlay", StringComparison.OrdinalIgnoreCase)),
            RenderPlan = new NativeMediaCoreRenderPlan
            {
                RenderPlanId = "test-plan",
                Layers = layers,
                Routes = []
            }
        };

    private static NativeMediaCoreStateSnapshot BuildCaptionSnapshot(
        string text,
        string speaker,
        double atMs,
        double confidence) =>
        new()
        {
            CaptionTrack = new NativeMediaCoreCaptionTrack
            {
                Enabled = true,
                Status = "live",
                CurrentCue = new NativeMediaCoreCaptionCue
                {
                    Text = text,
                    Speaker = speaker,
                    AtMs = atMs,
                    Confidence = confidence
                },
                LatencyMs = 180
            }
        };
}