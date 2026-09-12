using CoreVideoPro.WinUI.Models;

namespace CoreVideoPro.WinUI.Services;

/// <summary>
/// #479: Tiles manual slots and never-show persist Zoom's per-session user id
/// (<c>zoom:16778240</c>), which is reassigned every meeting. Persist a stable
/// key (SDK persistent id, else display name) and resolve to this meeting's
/// session id at use time. A leftover <c>zoom:&lt;sessionId&gt;</c> is never
/// applied to whoever inherited that integer in a later meeting.
/// </summary>
public static class TilesIdentityPolicy
{
    public const string ZoomPidPrefix = "zoom-pid:";
    public const string ZoomNamePrefix = "zoom-name:";
    public const string ZoomSessionPrefix = "zoom:";

    public sealed record Person(string SessionSourceId, string DisplayName, string? PersistentId = null);

    public sealed record StaleEntry(string PersistedKey, string Label, string Kind);

    public static Person FromParticipant(Participant participant) =>
        new(ShowInputRosterService.ZoomSourceId(participant.Id), participant.Name, participant.PersistentId);

    public static string Persist(string liveSourceId, IReadOnlyList<Person> roster)
    {
        if (string.IsNullOrEmpty(liveSourceId) || !liveSourceId.StartsWith(ZoomSessionPrefix, StringComparison.Ordinal) ||
            liveSourceId.StartsWith(ZoomPidPrefix, StringComparison.Ordinal) ||
            liveSourceId.StartsWith(ZoomNamePrefix, StringComparison.Ordinal))
        {
            return liveSourceId;
        }

        var person = roster.FirstOrDefault(entry =>
            string.Equals(entry.SessionSourceId, liveSourceId, StringComparison.Ordinal));
        if (person is null)
        {
            return liveSourceId;
        }

        if (!string.IsNullOrWhiteSpace(person.PersistentId))
        {
            return ZoomPidPrefix + person.PersistentId.Trim();
        }

        if (!string.IsNullOrWhiteSpace(person.DisplayName))
        {
            return ZoomNamePrefix + person.DisplayName.Trim();
        }

        return liveSourceId;
    }

    /// <summary>
    /// Live source id for membership, or null when the saved identity is not in
    /// this meeting. Legacy <c>zoom:&lt;sessionId&gt;</c> is only valid when it
    /// was saved in <paramref name="boundMeetingId"/> and that is still the
    /// current meeting. An unbound (pre-#479) session id is never applied.
    /// </summary>
    public static string? ResolveLiveSourceId(
        string? persisted,
        IReadOnlyList<Person> roster,
        string? currentMeetingId,
        string? boundMeetingId)
    {
        if (string.IsNullOrEmpty(persisted))
        {
            return persisted;
        }

        if (persisted.StartsWith(ZoomPidPrefix, StringComparison.Ordinal))
        {
            var pid = persisted[ZoomPidPrefix.Length..];
            return roster.FirstOrDefault(entry =>
                !string.IsNullOrWhiteSpace(entry.PersistentId) &&
                string.Equals(entry.PersistentId, pid, StringComparison.Ordinal))?.SessionSourceId;
        }

        if (persisted.StartsWith(ZoomNamePrefix, StringComparison.Ordinal))
        {
            var name = persisted[ZoomNamePrefix.Length..];
            var matches = roster
                .Where(entry => string.Equals(entry.DisplayName, name, StringComparison.OrdinalIgnoreCase))
                .Select(entry => entry.SessionSourceId)
                .Distinct(StringComparer.Ordinal)
                .ToList();
            return matches.Count == 1 ? matches[0] : null;
        }

        if (persisted.StartsWith(ZoomSessionPrefix, StringComparison.Ordinal) && persisted.Contains(':') &&
            persisted.Split(':').Length == 2)
        {
            // Per-session Zoom user ids are reassigned every meeting. Never apply a
            // leftover zoom:<id> to whoever inherited that integer (#479).
            return null;
        }

        return persisted;
    }

    public static string Label(string? persisted, IReadOnlyList<Person>? roster = null)
    {
        if (string.IsNullOrEmpty(persisted))
        {
            return "Unavailable source";
        }

        if (persisted.StartsWith(ZoomNamePrefix, StringComparison.Ordinal))
        {
            return persisted[ZoomNamePrefix.Length..];
        }

        if (persisted.StartsWith(ZoomPidPrefix, StringComparison.Ordinal))
        {
            var pid = persisted[ZoomPidPrefix.Length..];
            var named = roster?.FirstOrDefault(entry =>
                string.Equals(entry.PersistentId, pid, StringComparison.Ordinal));
            return named is null ? "saved Zoom participant" : named.DisplayName;
        }

        var live = roster?.FirstOrDefault(entry =>
            string.Equals(entry.SessionSourceId, persisted, StringComparison.Ordinal));
        return live?.DisplayName ?? "Unavailable source";
    }

    public static IReadOnlyList<StaleEntry> DescribeStale(
        DynamicGallerySettings settings,
        IReadOnlyList<Person> roster,
        string? currentMeetingId)
    {
        var stale = new List<StaleEntry>();
        var slots = settings.ManualSlots ?? [];
        for (var index = 0; index < slots.Count; index++)
        {
            var key = slots[index];
            if (string.IsNullOrEmpty(key))
            {
                continue;
            }

            if (ResolveLiveSourceId(key, roster, currentMeetingId, settings.BoundMeetingId) is null)
            {
                stale.Add(new StaleEntry(key, $"{index + 1}: {Label(key, roster)}: not in this meeting", "slot"));
            }
        }

        foreach (var key in settings.ExcludedSourceIds ?? [])
        {
            if (string.IsNullOrEmpty(key))
            {
                continue;
            }

            if (ResolveLiveSourceId(key, roster, currentMeetingId, settings.BoundMeetingId) is null)
            {
                stale.Add(new StaleEntry(key, $"{Label(key, roster)}: not in this meeting", "never-show"));
            }
        }

        return stale;
    }

    public static void ClearStale(
        DynamicGallerySettings settings,
        IReadOnlyList<Person> roster,
        string? currentMeetingId)
    {
        var slots = settings.ManualSlots ?? [];
        for (var index = 0; index < slots.Count; index++)
        {
            var key = slots[index];
            if (!string.IsNullOrEmpty(key) &&
                ResolveLiveSourceId(key, roster, currentMeetingId, settings.BoundMeetingId) is null)
            {
                slots[index] = null;
            }
        }

        settings.ExcludedSourceIds.RemoveAll(key =>
            !string.IsNullOrEmpty(key) &&
            ResolveLiveSourceId(key, roster, currentMeetingId, settings.BoundMeetingId) is null);
    }
}
