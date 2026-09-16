using CoreVideoPro.WinUI.Models;
using CoreVideoPro.WinUI.Services;
using Xunit;

namespace CoreVideoPro.WinUI.Tests;

public sealed class ParticipantMuteRefreshTests
{
    [Theory]
    [InlineData(FeedHealth.Live, false)]
    [InlineData(FeedHealth.Live, true)]
    [InlineData(FeedHealth.VideoOff, false)]
    [InlineData(FeedHealth.VideoOff, true)]
    public void MuteOnlyChangesReachMixerWithoutChangingParticipantSet(FeedHealth health, bool consoleMuted)
    {
        Participant Guest(bool muted) => new()
        {
            Id = "guest", Name = "Guest", Health = health, IsMuted = muted
        };
        IReadOnlyList<Participant> previous = [Guest(false)];
        var settings = new Dictionary<string, ParticipantAudioMix>
        {
            ["guest"] = new()
            {
                ParticipantId = "guest", Muted = consoleMuted, ManualGainDb = 3,
                OutputLevel = 0, GainDb = 0, NoiseSuppression = false, Status = "native-pcm"
            }
        };
        var row = new AudioParticipantRow();
        var notifications = new List<string?>();
        row.PropertyChanged += (_, args) => notifications.Add(args.PropertyName);

        foreach (var muted in new[] { true, false, true, false })
        {
            var current = ParticipantMapper.ParticipantsInRoom([Guest(muted)], "main");
            Assert.True(ParticipantMapper.HasMuteChanges(previous, current));
            var mix = Assert.Single(ProductionStateHelper.BuildAudioMixChannels(current, settings));
            row.SourceMuted = mix.SourceMuted;
            Assert.Equal(muted, row.SourceMuted);
            Assert.Equal(consoleMuted, mix.Muted);
            Assert.Equal(3, mix.ManualGainDb);
            Assert.Contains(nameof(AudioParticipantRow.SourceMuted), notifications);
            notifications.Clear();
            previous = current;
        }
    }

    [Fact]
    public void ReorderingOrLevelChangesDoNotTriggerMuteRefresh()
    {
        Participant Guest(string id, int level) => new() { Id = id, AudioLevel = level };
        Assert.False(ParticipantMapper.HasMuteChanges(
            [Guest("a", 0), Guest("b", 30)], [Guest("b", 70), Guest("a", 10)]));
    }

    [Fact]
    public void JoiningLeavingAndReplacedIdsRefreshMuteState()
    {
        var guest = new Participant { Id = "a" };
        Assert.True(ParticipantMapper.HasMuteChanges([], [guest]));
        Assert.True(ParticipantMapper.HasMuteChanges([guest], []));
        Assert.True(ParticipantMapper.HasMuteChanges([guest], [new Participant { Id = "b" }]));
    }
}
