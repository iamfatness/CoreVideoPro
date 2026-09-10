namespace CoreVideoPro.Control;

/// <summary>The most recent media-core session snapshot the shell received, carried as the core's
/// own JSON text rather than a projection of it.
///
/// Why text and not a typed model: the shell's typed <c>NativeMediaCoreStateSnapshot</c> only
/// binds the fields the shell happens to consume. Every other node the core publishes — encoder
/// evidence, real-time worker evidence, the program buffer, ISO streams, tiles, multiviewer,
/// browser sources — is dropped on deserialization. An observer needs what the core SAID, so the
/// raw document travels through unmodified except for redaction.
///
/// <paramref name="Json"/> is already redacted by the producer (the shell adapter) before it
/// reaches this layer: the control transports can never serve an unredacted snapshot because they
/// never hold one.
/// </summary>
/// <param name="Json">The redacted core snapshot JSON, or null when none is available.</param>
/// <param name="ReceivedUtc">When the SHELL received this snapshot — not when it was requested.
/// A consumer subtracts this from the response's <c>servedUtc</c> to tell snapshot age from
/// request age.</param>
/// <param name="UnavailableReason">Set iff <paramref name="Json"/> is null; says why.</param>
public sealed record NativeSnapshotObservation(
    string? Json,
    DateTimeOffset? ReceivedUtc,
    string? UnavailableReason)
{
    /// <summary>No core snapshot has reached the shell (engine off, core not started, or the
    /// first sync has not landed yet).</summary>
    public static NativeSnapshotObservation Unavailable(string reason) => new(null, null, reason);
}

/// <summary>Optional companion to <see cref="IControlSurface"/>: a surface that also observes a
/// running media core implements this so read-only transports can serve the core's own snapshot
/// without spawning a second core or issuing an extra round-trip to the running one.
///
/// Deliberately separate from <see cref="IControlSurface"/> so a surface that has no media core
/// (tests, the OSC-only host, a future headless surface) is unaffected — the router answers
/// "not observed" rather than failing to compile.
/// </summary>
public interface INativeSnapshotObserver
{
    /// <summary>The last snapshot the shell received, redacted. MUST be a cheap read of state the
    /// shell already holds — no core round-trip, no lock the render path needs.</summary>
    NativeSnapshotObservation GetNativeSnapshot();
}
