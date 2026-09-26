namespace CoreVideoPro.MediaCore.Models;

/// <summary>One observed ISO writer transition, keyed to its canonical source.</summary>
public sealed record IsoOutputLifecycleFact(string SourceId, string SessionId, string State, long Revision);
