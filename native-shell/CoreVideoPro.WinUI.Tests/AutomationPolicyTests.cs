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
    [InlineData("panel", 3, 3, 3)]
    [InlineData("interview", 4, 3, 2)]
    [InlineData("interview", 2, 5, 2)]
    [InlineData("intro", 4, 5, 1)]
    public void NativeShotAndBindingsStayTogetherRegardlessOfRawRosterCount(
        string scene, int threshold, int count, int slots)
    {
        var native = new NativeMediaCoreAutoProduction
        {
            RecommendedSceneId = scene, Confidence = 95, Rationale = "native floor",
            Policy = new() { PreferScreenShare = false, PanelGuestThreshold = threshold },
            SlotBindings = Enumerable.Range(0, slots).Select(i => new NativeDirectorSlotBinding
                { SlotIndex = i, PersonId = $"guest-{i}", SourceId = $"zoom:guest-{i}" }).ToArray()
        };
        var result = ProductionStateHelper.BuildAutomationRecommendation(native,
            Enumerable.Range(0, count).Select(i => Participant(i.ToString())).ToArray(),
            Scenes, false, threshold);
        Assert.Equal(scene, result.RecommendedSceneId);
        Assert.Equal(95, result.Confidence);
        Assert.Equal(Enumerable.Range(0, slots).Select(i => $"guest-{i}"),
            result.SlotBindings.Select(binding => binding.PersonId));
    }

    [Theory]
    [InlineData(true, 4)]
    [InlineData(false, 3)]
    public void StalePolicyCannotProduceAnActionableFallback(bool preferShare, int threshold)
    {
        var native = new NativeMediaCoreAutoProduction
        {
            RecommendedSceneId = "interview", Confidence = 95,
            Policy = new() { PreferScreenShare = false, PanelGuestThreshold = 4 },
            SlotBindings = [new() { SlotIndex = 0, PersonId = "a", SourceId = "zoom:a" },
                            new() { SlotIndex = 1, PersonId = "b", SourceId = "zoom:b" }]
        };
        var result = ProductionStateHelper.BuildAutomationRecommendation(native,
            [Participant("a"), Participant("b")], Scenes, preferShare, threshold);
        Assert.Equal(0, result.Confidence);
        Assert.Empty(result.SlotBindings);
        Assert.Contains("apply your settings", result.Reason);
    }

    [Fact]
    public void MissingNativePolicyAndDuplicatePeopleAreExplicitHolds()
    {
        var missing = ProductionStateHelper.BuildAutomationRecommendation(null,
            [Participant("a"), Participant("b")], Scenes);
        Assert.Equal(0, missing.Confidence);
        Assert.Contains("waiting for the native director", missing.Reason);
        var native = new NativeMediaCoreAutoProduction
        {
            RecommendedSceneId = "interview", Confidence = 95,
            SlotBindings = [new() { SlotIndex = 0, PersonId = "a", SourceId = "zoom:a" },
                            new() { SlotIndex = 1, PersonId = "a", SourceId = "capture:a" }]
        };
        var unqualified = NativeDirectorRecommendationPolicy.Resolve(native, Scenes, true, 4);
        Assert.Equal(0, unqualified.Confidence);
        Assert.Contains("does not report operator policy", unqualified.Reason);
        var duplicate = NativeDirectorRecommendationPolicy.Resolve(native with { Policy = new() }, Scenes, true, 4);
        Assert.Equal(0, duplicate.Confidence);
        Assert.Empty(duplicate.SlotBindings);
        Assert.Contains("duplicate", duplicate.Reason);
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
