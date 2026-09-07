using CoreVideoPro.MediaCore.Models;
using CoreVideoPro.WinUI.Services;
using Xunit;

namespace CoreVideoPro.WinUI.Tests;

/// <summary>Spec §6.2 — the MediaCore roster → engine <c>Participant</c> mapping. Every one of
/// these cases is a real Zoom state the core reports, and each nullable field has a DEFAULT that
/// must be the safe one: an unknown video state means "show them" (a guest wrongly hidden is a
/// show failure), an unknown mute state means "they can be heard".</summary>
public sealed class OhgParticipantMapperTests
{
    private static RawParticipantEvent Raw(
        string userId = "1001",
        string displayName = "Ada",
        string? role = null,
        bool? muted = null,
        bool? videoOn = null)
        => new()
        {
            UserId = userId,
            DisplayName = displayName,
            Role = role,
            Muted = muted,
            VideoOn = videoOn
        };

    [Fact]
    public void NullVideoOnMapsToVideoOn()
    {
        var mapped = OhgParticipantMapper.Map(new[] { Raw(videoOn: null) });

        Assert.True(Assert.Single(mapped).VideoOn);
    }

    [Fact]
    public void ExplicitVideoOffMapsToVideoOff()
    {
        var mapped = OhgParticipantMapper.Map(new[] { Raw(videoOn: false) });

        Assert.False(Assert.Single(mapped).VideoOn);
    }

    [Fact]
    public void MutedMapsToAudioOff()
    {
        var mapped = OhgParticipantMapper.Map(new[] { Raw(muted: true) });

        Assert.False(Assert.Single(mapped).AudioOn);
    }

    [Fact]
    public void UnknownMuteStateMapsToAudioOn()
    {
        var mapped = OhgParticipantMapper.Map(new[] { Raw(muted: null) });

        Assert.True(Assert.Single(mapped).AudioOn);
    }

    [Theory]
    [InlineData("host", 1)]
    [InlineData("HOST", 1)]
    [InlineData("cohost", 2)]
    [InlineData("CoHost", 2)]
    [InlineData("panelist", 0)]
    [InlineData("attendee", 0)]
    [InlineData(null, 0)]
    [InlineData("", 0)]
    public void RoleStringsMapToZoomRoleNumbers(string? role, int expected)
    {
        var mapped = OhgParticipantMapper.Map(new[] { Raw(role: role) });

        Assert.Equal(expected, Assert.Single(mapped).ZoomRole);
    }

    [Fact]
    public void ParticipantIdsAreCarriedVerbatimIncludingLeadingZeros()
    {
        var mapped = OhgParticipantMapper.Map(new[] { Raw(userId: "0007"), Raw(userId: "16778240") });

        Assert.Equal(new[] { "0007", "16778240" }, mapped.Select(p => p.ParticipantId));
    }

    [Fact]
    public void EveryMappedParticipantIsOnlineAndHandDown()
    {
        // "online" means "present in this snapshot" (spec §6.2) and hand-raised is not on the
        // core protocol at all — a recorded gap, never fabricated.
        var participant = Assert.Single(OhgParticipantMapper.Map(new[] { Raw() }));

        Assert.True(participant.Online);
        Assert.False(participant.HandRaised);
    }

    [Fact]
    public void DisplayNameIsCarriedAsRawName()
    {
        Assert.Equal("Ada Lovelace", Assert.Single(OhgParticipantMapper.Map(new[] { Raw(displayName: "Ada Lovelace") })).RawName);
    }

    [Fact]
    public void AnEmptyRosterMapsToAnEmptyList()
    {
        Assert.Empty(OhgParticipantMapper.Map(System.Array.Empty<RawParticipantEvent>()));
    }
}
