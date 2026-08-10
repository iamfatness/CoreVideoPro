using System.Text.Json;
using System.Text.Json.Serialization;
using CoreVideoPro.WinUI.Models;

namespace CoreVideoPro.WinUI.Services;

/// <summary>
/// Serializable snapshot of a single Input 1-10 slot assignment. Captures operator intent
/// (kind + chosen source ids) so a pre-built show survives an app restart even if the
/// referenced participant/device is not currently present.
/// </summary>
public sealed class ShowInputSlotRecord
{
    public int SlotNumber { get; set; }
    public ShowInputKind Kind { get; set; }
    public string? ParticipantId { get; set; }
    public string? CaptureDeviceId { get; set; }
    public string? AudioDeviceId { get; set; }
    public bool InShow { get; set; }
}

public sealed class ShowInputRosterSnapshot
{
    public int Version { get; set; } = 1;
    public List<ShowInputSlotRecord> Slots { get; set; } = [];
}

/// <summary>
/// Persists Input 1-10 slot assignments. Abstracted behind an interface so the persistence
/// logic is unit-testable without a running WinUI/ApplicationData host (tests use an
/// in-memory or temp-file implementation).
/// </summary>
public interface IShowInputRosterStore
{
    void Save(ShowInputRosterSnapshot snapshot);

    ShowInputRosterSnapshot? Load();
}

public static class ShowInputRosterSerializer
{
    private static readonly JsonSerializerOptions Options = new()
    {
        WriteIndented = true,
        Converters = { new JsonStringEnumConverter() }
    };

    public static string Serialize(ShowInputRosterSnapshot snapshot) =>
        JsonSerializer.Serialize(snapshot, Options);

    public static ShowInputRosterSnapshot? Deserialize(string? json)
    {
        if (string.IsNullOrWhiteSpace(json))
        {
            return null;
        }

        try
        {
            return JsonSerializer.Deserialize<ShowInputRosterSnapshot>(json, Options);
        }
        catch (JsonException)
        {
            return null;
        }
    }

    public const int CurrentVersion = 2;

    /// <summary>Project the live slots into a serializable snapshot.</summary>
    public static ShowInputRosterSnapshot CaptureFrom(IEnumerable<ShowInputSlot> slots) =>
        GarbageCollect(new ShowInputRosterSnapshot
        {
            Version = CurrentVersion,
            Slots = slots
                .Select(slot => new ShowInputSlotRecord
                {
                    SlotNumber = slot.SlotNumber,
                    Kind = slot.Kind,
                    ParticipantId = slot.ParticipantId,
                    CaptureDeviceId = slot.CaptureDeviceId,
                    AudioDeviceId = slot.AudioDeviceId,
                    InShow = slot.InShow
                })
                .ToList()
        });

    /// <summary>
    /// Lifecycle L7 (source-lifecycle-spec): self-healing garbage collection of
    /// the persisted roster. Parked (non-in-show) slots that DUPLICATE the
    /// source of a lower-numbered slot are reset to Unassigned, so duplicate
    /// ghosts can never accumulate across sessions even if the runtime sweep
    /// did not run. In-show slots are always kept (operator intent wins). The
    /// snapshot is versioned so future migrations are explicit.
    /// </summary>
    public static ShowInputRosterSnapshot GarbageCollect(ShowInputRosterSnapshot snapshot)
    {
        if (snapshot?.Slots is not { Count: > 0 })
        {
            return snapshot ?? new ShowInputRosterSnapshot { Version = CurrentVersion };
        }

        var ordered = snapshot.Slots.OrderBy(record => record.SlotNumber).ToList();
        var seen = new HashSet<string>(StringComparer.Ordinal);
        foreach (var record in ordered)
        {
            var key = SourceKey(record);
            if (key is null)
            {
                continue;  // unassigned
            }
            if (!seen.Add(key) && !record.InShow)
            {
                // Duplicate of an earlier slot's source, and parked: drop it.
                record.Kind = ShowInputKind.Unassigned;
                record.ParticipantId = null;
                record.CaptureDeviceId = null;
                record.AudioDeviceId = null;
            }
        }

        snapshot.Version = CurrentVersion;
        return snapshot;
    }

    private static string? SourceKey(ShowInputSlotRecord record) =>
        record.Kind switch
        {
            ShowInputKind.Unassigned => null,
            ShowInputKind.ZoomParticipant or ShowInputKind.Media =>
                string.IsNullOrWhiteSpace(record.ParticipantId) ? null : $"{record.Kind}:{record.ParticipantId}",
            _ => string.IsNullOrWhiteSpace(record.CaptureDeviceId) ? null : $"{record.Kind}:{record.CaptureDeviceId}"
        };

    /// <summary>
    /// Re-apply a saved snapshot onto the live slots. The slot's intent (kind + chosen source
    /// ids) is always restored; whether the referenced source is currently present is left to
    /// the roster refresh, which marks unresolved sources as unavailable rather than dropping
    /// the assignment.
    /// </summary>
    public static void ApplyTo(IReadOnlyList<ShowInputSlot> slots, ShowInputRosterSnapshot? snapshot)
    {
        if (snapshot?.Slots is not { Count: > 0 })
        {
            return;
        }

        // Lifecycle L7: garbage-collect duplicate ghosts on load, so a stale
        // pre-L7 roster file self-heals the first time it is read.
        snapshot = GarbageCollect(snapshot);

        using var _ = ShowInputWriteScope.Enter("roster-load");

        var bySlot = snapshot.Slots
            .GroupBy(record => record.SlotNumber)
            .ToDictionary(group => group.Key, group => group.First());

        foreach (var slot in slots)
        {
            if (!bySlot.TryGetValue(slot.SlotNumber, out var record))
            {
                continue;
            }

            slot.Kind = record.Kind;
            slot.ParticipantId = record.ParticipantId;
            slot.CaptureDeviceId = record.CaptureDeviceId;
            slot.AudioDeviceId = record.AudioDeviceId;
            slot.InShow = record.InShow;
        }
    }
}

/// <summary>
/// JSON-file backed store under a caller-supplied folder. The WinUI shell points this at
/// ApplicationData.Current.LocalFolder; tests can point it at a temp directory.
/// </summary>
public sealed class FileShowInputRosterStore : IShowInputRosterStore
{
    public const string DefaultFileName = "show-input-roster.json";

    private readonly string _filePath;

    public FileShowInputRosterStore(string folderPath, string? fileName = null)
    {
        _filePath = Path.Combine(folderPath, fileName ?? DefaultFileName);
    }

    public void Save(ShowInputRosterSnapshot snapshot)
    {
        var directory = Path.GetDirectoryName(_filePath);
        if (!string.IsNullOrEmpty(directory))
        {
            Directory.CreateDirectory(directory);
        }

        File.WriteAllText(_filePath, ShowInputRosterSerializer.Serialize(snapshot));
    }

    public ShowInputRosterSnapshot? Load()
    {
        if (!File.Exists(_filePath))
        {
            return null;
        }

        try
        {
            return ShowInputRosterSerializer.Deserialize(File.ReadAllText(_filePath));
        }
        catch (IOException)
        {
            return null;
        }
        catch (UnauthorizedAccessException)
        {
            return null;
        }
    }
}

/// <summary>In-memory store for tests / hosts without a writable local folder.</summary>
public sealed class InMemoryShowInputRosterStore : IShowInputRosterStore
{
    private ShowInputRosterSnapshot? _snapshot;

    public void Save(ShowInputRosterSnapshot snapshot) => _snapshot = snapshot;

    public ShowInputRosterSnapshot? Load() => _snapshot;
}
