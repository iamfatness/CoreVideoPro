using CommunityToolkit.Mvvm.ComponentModel;

namespace CoreVideoPro.WinUI.Models;

/// <summary>
/// A destination bus in the routing matrix (a matrix column). For audio these are
/// the program/ISO/monitor/stream mix buses a source can be sent to. The header
/// and every crosspoint cell share ONE instance, so a notifying <see cref="Label"/>
/// lets a rename update the whole column at once (audio-tab-redesign 4).
/// </summary>
public sealed partial class RoutingBus : ObservableObject
{
    public RoutingBus(string id, string label)
    {
        Id = id;
        _label = label;
        Purpose = BusPurposeText(id);
    }

    public string Id { get; }

    [ObservableProperty]
    private string _label;

    // Lifecycle L6: aux/custom buses are deletable AND renamable; fixed program buses are not.
    public bool IsRemovable => Id.StartsWith("aux-", StringComparison.Ordinal) || Id.StartsWith("bus-", StringComparison.Ordinal);

    /// <summary>
    /// U2: what this bus is FOR, in operator words. Shown on the bus cards and
    /// as the matrix column-header tooltip so the bus model explains itself.
    /// </summary>
    public string Purpose { get; }

    /// <summary>U2: how many sources currently route here (kept fresh by the matrix VM).</summary>
    [ObservableProperty]
    [NotifyPropertyChangedFor(nameof(RoutedSummary))]
    private int _routedSourceCount;

    public string RoutedSummary => RoutedSourceCount switch
    {
        0 => "Nothing routed",
        1 => "1 source routed",
        _ => $"{RoutedSourceCount} sources routed",
    };

    private static string BusPurposeText(string id)
    {
        if (id.StartsWith("iso-", StringComparison.OrdinalIgnoreCase))
        {
            return "Isolated recording feed — carries ONE source for a clean per-source recording.";
        }

        if (id.StartsWith("aux-", StringComparison.Ordinal) || id.StartsWith("bus-", StringComparison.Ordinal))
        {
            return "Aux send — build a custom mix (a guest's mix-minus, an effects send, a second output).";
        }

        return id switch
        {
            "master" => "The final mix. Everything the audience hears passes through here.",
            "pgm-l" => "Program left — carries the mastered mix to outputs (record, stream, virtual camera).",
            "pgm-r" => "Program right — carries the mastered mix to outputs (record, stream, virtual camera).",
            "mon" => "Monitor — what YOU hear in your headphones. Never sent to the audience.",
            "stream" => "Stream — the mix sent to your live-stream destinations.",
            _ => "Custom bus.",
        };
    }
}

/// <summary>
/// A routable source (a matrix row) — an assigned Input (1–10) or a synthetic
/// source such as the Zoom program mix or media playback.
/// </summary>
public sealed record RoutingSource(string Id, string Label, bool DefaultUnrouted = false);

/// <summary>
/// A video destination in the routing matrix (a matrix column) — an ISO record
/// channel, a multiview tile, or an aux/stream send. Video crosspoints are
/// on/off only (no gain), unlike the audio buses.
/// </summary>
public sealed record RoutingDestination(string Id, string Label);

/// <summary>
/// A mixer processing target: either a source/channel strip or an output bus.
/// Insert controls use this so EQ/compression/VST slots are applied to the right
/// channel, aux, bus, or master path instead of an implicit selected participant.
/// </summary>
public sealed class AudioProcessingTargetOption
{
    public required string Id { get; init; }
    public required string Label { get; init; }
    public required string Kind { get; init; }
    public required string Detail { get; init; }
    public required string InsertLabel { get; init; }
}
