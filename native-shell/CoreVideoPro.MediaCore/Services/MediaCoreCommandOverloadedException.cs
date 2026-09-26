namespace CoreVideoPro.MediaCore.Services;

/// <summary>A new command was not admitted to the bounded core queue; no mutation was sent.</summary>
public sealed class MediaCoreCommandOverloadedException : InvalidOperationException
{
    public MediaCoreCommandOverloadedException()
        : base("Media core command queue is full; read current state and retry the edit.") { }
}
