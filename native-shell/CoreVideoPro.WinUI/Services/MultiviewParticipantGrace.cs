using CoreVideoPro.WinUI.Models;

namespace CoreVideoPro.WinUI.Services;

/// <summary>
/// #725: keeps an in-show Zoom slot on the multiview through a momentary roster dip.
///
/// Owner report 2026-09-30: on every Take the shell resolved Show Input slots against a
/// participant list that was missing the producer (the app's own camera-off user) for one
/// apply, so <c>set-multiview-layout</c> went out with one tile fewer and the wall reflowed
/// (5+4 to 4+4 and back) on every cut. The source of the short list was not identified - it
/// did not reproduce after a restart - so this does two things:
/// <list type="number">
/// <item>A participant assigned to an in-show slot who was present within
/// <see cref="Grace"/> is kept (with their last-seen details) when a list arrives without
/// them. Someone who really leaves is still dropped once the grace runs out, on the next
/// layout build (the spine sync rebuilds it continuously).</item>
/// <item>Every miss is logged regardless of verbose diagnostics, once per dip per slot: the
/// caller that built the list, its size, and whether the core's own latest roster contains
/// the participant - the one fact that splits a shell-side cause from a core-side one.</item>
/// </list>
/// UI thread only (every caller builds layouts on the dispatcher).
/// </summary>
internal sealed class MultiviewParticipantGrace
{
    public static readonly TimeSpan Grace = TimeSpan.FromSeconds(1);

    private readonly Func<long> _clockMs;
    private readonly Action<string> _log;
    private readonly Dictionary<string, (Participant Participant, long SeenAtMs)> _lastSeen = new(StringComparer.Ordinal);
    private readonly HashSet<string> _reportedMisses = new(StringComparer.Ordinal);

    public MultiviewParticipantGrace(Func<long>? clockMs = null, Action<string>? log = null)
    {
        _clockMs = clockMs ?? (() => Environment.TickCount64);
        _log = log ?? LaunchLog.Write;
    }

    /// <summary>
    /// The participant list to resolve <paramref name="slots"/> against: <paramref name="current"/>
    /// plus any in-show slot's participant who is missing from it but was seen within the grace.
    /// </summary>
    public IReadOnlyList<Participant> Resolve(
        IReadOnlyList<ShowInputSlot> slots,
        IReadOnlyList<Participant> current,
        string caller,
        Func<string, bool>? coreRosterHas = null)
    {
        var now = _clockMs();
        foreach (var participant in current)
        {
            if (!string.IsNullOrEmpty(participant.Id))
                _lastSeen[participant.Id] = (participant, now);
        }

        var present = current.Select(participant => participant.Id).ToHashSet(StringComparer.Ordinal);
        List<Participant>? held = null;
        foreach (var slot in slots)
        {
            if (!slot.InShow || slot.Kind != ShowInputKind.ZoomParticipant ||
                slot.ParticipantId is not { Length: > 0 } pid || present.Contains(pid))
            {
                continue;
            }

            var seen = _lastSeen.TryGetValue(pid, out var last);
            var ageMs = seen ? now - last.SeenAtMs : -1;
            var holding = seen && ageMs <= (long)Grace.TotalMilliseconds;
            var key = $"{slot.SlotNumber}:{pid}:{(holding ? "hold" : "drop")}";
            // An empty roster is "not in a meeting", not a dip: persisted slots name nobody yet.
            if (current.Count > 0 && _reportedMisses.Add(key))
            {
                var core = coreRosterHas is null ? "unknown" : coreRosterHas(pid) ? "yes" : "no";
                _log($"mv-roster-miss: slot{slot.SlotNumber} pid={pid} missing from {caller} list " +
                     $"(n={current.Count}); lastSeenMs={ageMs}; coreRosterHasIt={core}; " +
                     (holding ? $"holding on the multiview for up to {Grace.TotalMilliseconds:0}ms" : "dropped from the multiview"));
            }

            if (holding)
            {
                held ??= [];
                held.Add(last.Participant);
                present.Add(pid);
            }
        }

        // A participant present again re-arms reporting for their next dip.
        _reportedMisses.RemoveWhere(key =>
        {
            var parts = key.Split(':');
            return parts.Length == 3 && current.Any(participant => participant.Id == parts[1]);
        });
        return held is null ? current : [.. current, .. held];
    }
}
