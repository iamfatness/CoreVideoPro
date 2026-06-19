namespace CoreVideoPro.WinUI.Models;

/// <summary>
/// A destination bus in the routing matrix (a matrix column). For audio these are
/// the program/ISO/monitor/stream mix buses a source can be sent to.
/// </summary>
public sealed record RoutingBus(string Id, string Label);

/// <summary>
/// A routable source (a matrix row) — an assigned Input (1–10) or a synthetic
/// source such as the Zoom program mix or media playback.
/// </summary>
public sealed record RoutingSource(string Id, string Label);

/// <summary>
/// A video destination in the routing matrix (a matrix column) — an ISO record
/// channel, a multiview tile, or an aux/stream send. Video crosspoints are
/// on/off only (no gain), unlike the audio buses.
/// </summary>
public sealed record RoutingDestination(string Id, string Label);
