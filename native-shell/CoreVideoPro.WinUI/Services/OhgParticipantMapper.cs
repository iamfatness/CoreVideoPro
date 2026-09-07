using CoreVideoPro.MediaCore.Models;
using CoreVideoPro.ShowEngine;

namespace CoreVideoPro.WinUI.Services;

/// <summary>
/// Spec §6.2 — the MediaCore Zoom roster (<see cref="RawParticipantEvent"/>) projected onto the
/// show engine's <c>Participant</c> contract (<see cref="ShowEngineParticipant"/>).
///
/// Pure and static so the whole roster wire shape is testable without a core, a bridge, or a
/// ViewModel. Two rules worth naming, because both nullable fields default to the SAFE answer
/// rather than to <c>false</c>:
/// <list type="bullet">
/// <item><c>VideoOn == null</c> ⇒ <c>true</c>. "The core has not told us yet" must not read as
/// "this guest's camera is off": the engine would skip them when filling boxes, and a guest
/// wrongly left out of a look is a show failure that no one sees until air.</item>
/// <item><c>Muted == null</c> ⇒ <c>audioOn: true</c>. Same reasoning, mirrored.</item>
/// </list>
/// <c>online</c> is always true — presence in this snapshot IS being online; the engine diffs
/// successive rosters to learn who left. <c>handRaised</c> is always false: raise-hand is not on
/// the core protocol (spec §1 non-goals, a RECORDED gap) and a fabricated value is worse than a
/// known-missing one.
/// </summary>
public static class OhgParticipantMapper
{
    /// <summary>Map one core roster snapshot. Ids are carried VERBATIM — Zoom user ids are
    /// strings, may carry leading zeros, and are the same strings <c>input.assign zoom:&lt;pid&gt;</c>
    /// uses; parsing them to a number would silently break the join with the shell's own slots.</summary>
    public static IReadOnlyList<ShowEngineParticipant> Map(IReadOnlyList<RawParticipantEvent> participants)
    {
        if (participants is null || participants.Count == 0)
        {
            return System.Array.Empty<ShowEngineParticipant>();
        }

        var mapped = new List<ShowEngineParticipant>(participants.Count);
        foreach (var participant in participants)
        {
            if (participant is null)
            {
                continue;
            }

            mapped.Add(new ShowEngineParticipant(
                ParticipantId: participant.UserId,
                RawName: participant.DisplayName,
                Online: true,
                VideoOn: participant.VideoOn ?? true,
                AudioOn: !(participant.Muted ?? false),
                HandRaised: false,
                ZoomRole: ZoomRole(participant.Role)));
        }

        return mapped;
    }

    /// <summary>The core's role string → the engine's numeric Zoom role. Case-insensitive because
    /// the core mirrors whatever the SDK reported; anything unrecognized is 0 (attendee/panelist),
    /// never a guess at a privileged role.</summary>
    public static int ZoomRole(string? role) => role?.Trim().ToLowerInvariant() switch
    {
        "host" => 1,
        "cohost" => 2,
        _ => 0
    };
}
