namespace CoreVideoPro.WinUI.Models;

/// <summary>
/// A destination bus in the routing matrix (a matrix column). For audio these are
/// the program/ISO/monitor/stream mix buses a source can be sent to.
/// </summary>
public sealed record RoutingBus(string Id, string Label)
{
    // Lifecycle L6: aux/custom buses are deletable; fixed program buses are not.
    public bool IsRemovable => Id.StartsWith("aux-", StringComparison.Ordinal) || Id.StartsWith("bus-", StringComparison.Ordinal);
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
