using CoreVideoPro.WinUI.Models;
using CoreVideoPro.MediaCore.Models;
using Xunit;

namespace CoreVideoPro.WinUI.Tests;

public sealed class AutomationPolicyTests
{
    private static readonly IReadOnlyList<Scene> Scenes =
    [
        new() { Id = "intro", Name = "Intro", Layout = "host-focus" },
        new() { Id = "interview", Name = "Interview", Layout = "two-up" },
        new() { Id = "speaker-slides", Name = "Speaker + Slides", Layout = "speaker-slides" },
        new() { Id = "panel", Name = "Panel", Layout = "smart-grid" }
    ];

    [Fact]
    public void RecommendationHonorsScreenSharePreference()
    {
        var participants = new[]
        {
            Participant("host", isScreenSharing: true),
            Participant("guest")
        };

        var preferred = ProductionStateHelper.BuildAutomationRecommendation(
            participants,
            Scenes,
            preferScreenShare: true,
            panelParticipantThreshold: 4);
        var ignored = ProductionStateHelper.BuildAutomationRecommendation(
            participants,
            Scenes,
            preferScreenShare: false,
            panelParticipantThreshold: 4);

        Assert.Equal("speaker-slides", preferred.RecommendedSceneId);
        Assert.Equal("interview", ignored.RecommendedSceneId);
    }

    [Fact]
    public void RecommendationHonorsPanelParticipantThreshold()
    {
        var participants = new[]
        {
            Participant("one"),
            Participant("two"),
            Participant("three")
        };

        var thresholdThree = ProductionStateHelper.BuildAutomationRecommendation(
            participants,
            Scenes,
            preferScreenShare: true,
            panelParticipantThreshold: 3);
        var thresholdFour = ProductionStateHelper.BuildAutomationRecommendation(
            participants,
            Scenes,
            preferScreenShare: true,
            panelParticipantThreshold: 4);

        Assert.Equal("panel", thresholdThree.RecommendedSceneId);
        Assert.Equal("interview", thresholdFour.RecommendedSceneId);
    }

    [Theory]
    [InlineData("speaker-slides", false, 4, 2, "interview")]
    [InlineData("panel", true, 4, 3, "interview")]
    [InlineData("interview", true, 2, 2, "panel")]
    [InlineData("intro", true, 4, 5, "intro")]
    public void NativeRecommendationHonorsOperatorPolicyWithoutLosingDominantSpeakerDecision(
        string nativeScene, bool preferShare, int threshold, int count, string expected)
    {
        var native = new NativeMediaCoreAutoProduction
        {
            RecommendedSceneId = nativeScene,
            Confidence = 95,
            RuleId = nativeScene == "intro" ? "single-speaker" : "test",
            Rationale = "native recommendation"
        };
        var participants = Enumerable.Range(0, count)
            .Select(i => Participant(i.ToString(), nativeScene == "speaker-slides" && i == 0)).ToArray();
        var recommendation = ProductionStateHelper.BuildAutomationRecommendation(
            native, participants, Scenes, preferShare, threshold);
        Assert.Equal(expected, recommendation.RecommendedSceneId);
        if (nativeScene == "intro") Assert.Equal(95, recommendation.Confidence);
    }

    private static Participant Participant(string id, bool isScreenSharing = false) => new()
    {
        Id = id,
        Name = id,
        Role = ParticipantRole.Guest,
        Health = FeedHealth.Live,
        IsScreenSharing = isScreenSharing
    };

    // #1070: catalog scenes are deletable; a preset whose scene is gone cues the first scene.
    [Fact]
    public void RecommendationFallsBackToTheFirstSceneWhenThePresetSceneWasRemoved()
    {
        var withoutPanel = Scenes.Where(scene => scene.Id != "panel").ToList();
        var participants = new[] { Participant("a"), Participant("b"), Participant("c"), Participant("d") };

        var recommendation = ProductionStateHelper.BuildAutomationRecommendation(
            participants, withoutPanel, preferScreenShare: true, panelParticipantThreshold: 4);

        Assert.Equal("intro", recommendation.RecommendedSceneId);
        Assert.Contains("panel", recommendation.Reason);
        // The fallback is shown, never cut: MagicSceneCoordinator holds on confidence <= 0 and
        // auto-Takes only at >= 88 with bindings, so neither gate can open on it.
        Assert.Equal(0, recommendation.Confidence);
        Assert.Equal("hold", recommendation.Action);
        Assert.Empty(recommendation.SlotBindings);

        var intact = ProductionStateHelper.BuildAutomationRecommendation(
            participants, Scenes, preferScreenShare: true, panelParticipantThreshold: 4);
        Assert.Equal("panel", intact.RecommendedSceneId);
    }
}
