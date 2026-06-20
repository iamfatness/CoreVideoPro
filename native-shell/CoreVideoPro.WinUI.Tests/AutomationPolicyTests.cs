using CoreVideoPro.WinUI.Models;
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

    private static Participant Participant(string id, bool isScreenSharing = false) => new()
    {
        Id = id,
        Name = id,
        Role = ParticipantRole.Guest,
        Health = FeedHealth.Live,
        IsScreenSharing = isScreenSharing
    };
}
