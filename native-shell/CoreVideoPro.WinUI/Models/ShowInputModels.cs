namespace CoreVideoPro.WinUI.Models;

public enum ShowInputKind
{
    Unassigned,
    ZoomParticipant,
    Blackmagic,
    Aja,
    UvcWebcam,
    Screen,
    SrtIngest,
    Media,
    Browser
}

public sealed class ShowInputKindOption
{
    public required ShowInputKind Value { get; init; }
    public required string Label { get; init; }
}

public sealed class ShowInputSourceOption
{
    public required string Value { get; init; }
    public required string Label { get; init; }

    /// <summary>Category for the source-picker menu ("Zoom", "Camera", "Screen", "SRT",
    /// "Media") — the picker shows one submenu per group so long device lists stay
    /// filterable at pick time. Empty for entries outside any group (e.g. Missing).</summary>
    public string Group { get; init; } = string.Empty;
}

/// <summary>A multiviewer drag-drop reorder request: swap the Show Input assignments of two
/// slots (1-based slot numbers). Raised by ShowMultiviewHost when a source tile is dropped
/// onto another source tile.</summary>
public sealed record MultiviewTileSwapRequest(int FromSlot, int ToSlot);

/// <summary>
/// Ambient "who is writing the Show Input slots" reason, logged by the
/// <see cref="ShowInputSlot"/> setters — the single choke point every slot-assignment
/// write in the app lands on. Exists because THREE separate background writers
/// (auto-assign refill, EnsureAssignedSlotsForInShow, the dual-capture stuffer) each
/// took a live-meeting debugging round to name (2026-08-09); with this, an
/// unexplained assignment is one grep of launch.log ("slot-write:") instead of a day.
/// Slot writes are UI-thread-only (the 0xc000027b rules already require it), so a
/// plain static is sufficient. Scopes NEST by chaining reasons ("control-api&gt;
/// operator-picker") so an outer entry point stays visible through inner setters.
/// Any write outside a scope logs as UNTRACKED — treat an UNTRACKED slot-write in
/// the log as a bug: name the writer by wrapping it in a scope.
/// </summary>
public static class ShowInputWriteScope
{
    public const string Untracked = "UNTRACKED";

    // ThreadStatic (not a plain static): slot writes are UI-thread-only in the app, so
    // per-thread is equivalent there — and it keeps parallel test runners from
    // interleaving each other's reasons. A [ThreadStatic] initializer only runs on the
    // first thread, hence the null-coalesce instead of a field initializer.
    [ThreadStatic]
    private static string? _reason;

    public static string Reason => _reason ?? Untracked;

    public static IDisposable Enter(string reason)
    {
        var prior = _reason;
        _reason = prior is null or Untracked ? reason : $"{prior}>{reason}";
        return new Restore(prior);
    }

    private sealed class Restore(string? prior) : IDisposable
    {
        public void Dispose() => _reason = prior;
    }
}

public sealed partial class ShowInputSlot : CommunityToolkit.Mvvm.ComponentModel.ObservableObject
{
    public int SlotNumber { get; init; }

    [CommunityToolkit.Mvvm.ComponentModel.ObservableProperty]
    private ShowInputKind _kind = ShowInputKind.Unassigned;

    [CommunityToolkit.Mvvm.ComponentModel.ObservableProperty]
    private string? _participantId;

    [CommunityToolkit.Mvvm.ComponentModel.ObservableProperty]
    private string? _captureDeviceId;

    [CommunityToolkit.Mvvm.ComponentModel.ObservableProperty]
    private string? _audioDeviceId;

    [CommunityToolkit.Mvvm.ComponentModel.ObservableProperty]
    private bool _inShow;

    public string SlotLabel => $"Input {SlotNumber:00}";

    public string KindLabel => Kind switch
    {
        ShowInputKind.ZoomParticipant => "Zoom",
        ShowInputKind.Blackmagic => "Blackmagic",
        ShowInputKind.Aja => "AJA",
        ShowInputKind.UvcWebcam => "UVC webcam",
        ShowInputKind.Screen => "Screen",
        ShowInputKind.SrtIngest => "SRT ingest",
        ShowInputKind.Media => "Media",
        ShowInputKind.Browser => "Browser",
        _ => "Unassigned"
    };

    public bool IsAssigned => Kind switch
    {
        ShowInputKind.ZoomParticipant or ShowInputKind.Media => !string.IsNullOrWhiteSpace(ParticipantId),
        ShowInputKind.Blackmagic or ShowInputKind.Aja or ShowInputKind.UvcWebcam or ShowInputKind.Screen or ShowInputKind.SrtIngest or ShowInputKind.Browser => !string.IsNullOrWhiteSpace(CaptureDeviceId),
        _ => false
    };

    public bool IsSourcePickerEnabled =>
        Kind is ShowInputKind.ZoomParticipant or ShowInputKind.Blackmagic or ShowInputKind.Aja or ShowInputKind.UvcWebcam or ShowInputKind.Screen or ShowInputKind.SrtIngest or ShowInputKind.Media or ShowInputKind.Browser;

    public bool IsAudioPickerEnabled =>
        Kind is ShowInputKind.UvcWebcam or ShowInputKind.Blackmagic or ShowInputKind.Aja;

    partial void OnKindChanged(ShowInputKind value)
    {
        if (value == ShowInputKind.Unassigned)
        {
            ParticipantId = null;
            CaptureDeviceId = null;
            AudioDeviceId = null;
            InShow = false;
        }
        else if (value is ShowInputKind.ZoomParticipant or ShowInputKind.Media)
        {
            CaptureDeviceId = null;
            AudioDeviceId = null;
        }
        else
        {
            ParticipantId = null;
            if (value is ShowInputKind.SrtIngest or ShowInputKind.Browser)
            {
                AudioDeviceId = null;  // no paired-audio concept (browser audio is BR-3)
            }
        }

        OnPropertyChanged(nameof(KindLabel));
        OnPropertyChanged(nameof(IsAssigned));
        OnPropertyChanged(nameof(IsSourcePickerEnabled));
        OnPropertyChanged(nameof(IsAudioPickerEnabled));
    }

    partial void OnParticipantIdChanged(string? value) => OnPropertyChanged(nameof(IsAssigned));

    partial void OnCaptureDeviceIdChanged(string? value) => OnPropertyChanged(nameof(IsAssigned));

    partial void OnAudioDeviceIdChanged(string? value) => OnPropertyChanged(nameof(IsAudioPickerEnabled));

    partial void OnInShowChanged(bool value) => OnPropertyChanged(nameof(IsAssigned));

    // ── the slot-write choke point ────────────────────────────────────────────────
    // EVERY assignment write in the app funnels through these setters (the editor VM
    // forwards to this model; services/coordinators write it directly). Log each real
    // change with the ambient ShowInputWriteScope reason so the NEXT mystery writer —
    // this defect took three background writers and three live-meeting reports to
    // pin — is identifiable from launch.log alone. These fire only on actual value
    // changes (generated setters early-return on equality), so the volume is operator/
    // roster-churn rate, never snapshot rate.

    partial void OnKindChanged(ShowInputKind oldValue, ShowInputKind newValue) =>
        LogSlotWrite("Kind", oldValue.ToString(), newValue.ToString());

    partial void OnParticipantIdChanged(string? oldValue, string? newValue) =>
        LogSlotWrite("ParticipantId", oldValue, newValue);

    partial void OnCaptureDeviceIdChanged(string? oldValue, string? newValue) =>
        LogSlotWrite("CaptureDeviceId", oldValue, newValue);

    partial void OnInShowChanged(bool oldValue, bool newValue) =>
        LogSlotWrite("InShow", oldValue ? "true" : "false", newValue ? "true" : "false");

    private void LogSlotWrite(string field, string? oldValue, string? newValue) =>
        CoreVideoPro.WinUI.LaunchLog.Write(
            $"slot-write: slot{SlotNumber} {field} '{oldValue ?? ""}'->'{newValue ?? ""}' by={ShowInputWriteScope.Reason}");
}
