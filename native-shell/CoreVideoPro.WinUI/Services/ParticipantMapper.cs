using CoreVideoPro.MediaCore.Services;
using CoreVideoPro.WinUI.Models;

namespace CoreVideoPro.WinUI.Services;

public static class ParticipantMapper
{
    public static Participant ToParticipant(LiveProductionSync.LiveProductionParticipantContext context) =>
        new()
        {
            Id = context.Id,
            Name = context.Name,
            Title = context.Title,
            Role = ParseRole(context.RoleLabel),
            BreakoutRoomId = context.BreakoutRoomId,
            BreakoutRoomName = context.BreakoutRoomName,
            IsActiveSpeaker = context.IsActiveSpeaker,
            IsMuted = context.IsMuted,
            IsScreenSharing = context.IsScreenSharing,
            AudioLevel = context.AudioLevel,
            Health = ParseHealth(context.HealthLabel)
        };

    public static IReadOnlyList<Participant> ToParticipants(
        IReadOnlyList<LiveProductionSync.LiveProductionParticipantContext> participants) =>
        participants.Select(ToParticipant).ToList();

    // All participants in the room (video on OR off). Used for the Sources/Inputs
    // picker so an operator can assign a participant to an Input slot even when
    // their camera is momentarily off — the video flows once they enable it.
    public static IReadOnlyList<Participant> ParticipantsInRoom(
        IReadOnlyList<Participant> participants,
        string roomId) =>
        participants
            .Where(participant =>
                string.IsNullOrEmpty(participant.BreakoutRoomId) || participant.BreakoutRoomId == roomId)
            .ToList();

    public static IReadOnlyList<Participant> VideoParticipantsInRoom(
        IReadOnlyList<Participant> participants,
        string roomId) =>
        participants
            // The Zoom engine roster does not report a breakout-room id, so live
            // participants arrive with an empty BreakoutRoomId. Treat empty/unknown
            // as "the current room" — otherwise no live participant is ever
            // selectable as an input source.
            .Where(participant =>
                (string.IsNullOrEmpty(participant.BreakoutRoomId) || participant.BreakoutRoomId == roomId)
                && participant.Health != FeedHealth.VideoOff)
            .ToList();

    private static ParticipantRole ParseRole(string roleLabel) =>
        roleLabel.Trim().ToLowerInvariant() switch
        {
            "host" => ParticipantRole.Host,
            "presenter" or "speaker" => ParticipantRole.Presenter,
            "panelist" or "panel" => ParticipantRole.Panelist,
            _ => ParticipantRole.Guest
        };

    private static FeedHealth ParseHealth(string healthLabel) =>
        healthLabel.Trim().ToLowerInvariant() switch
        {
            "video-off" => FeedHealth.VideoOff,
            "low-resolution" => FeedHealth.LowResolution,
            "recovering" => FeedHealth.Recovering,
            _ => FeedHealth.Live
        };
}