using CoreVideoPro.Control;

namespace CoreVideoPro.WinUI.Services;

/// <summary>transport.ndi.graphicsKey.set: validates the whole request before anything
/// changes, then applies the toggle and the name as ONE edit (the caller saves and syncs once), so
/// a name+on request can never fire two output syncs that flap the key sender.</summary>
internal static class NdiGraphicsKeyControl
{
    /// <param name="apply">Receives (on, nameOrNullToKeep). Called at most once, only on success.</param>
    internal static ControlInvokeResult Set(bool on, string? name, bool ndiEnabled, bool? coreHasCapability,
        string? programName, string? currentGraphicsName, Action<bool, string?> apply)
    {
        var nextName = name is null ? currentGraphicsName : name.Trim();
        // A supplied name is validated even when turning the key off, so an invalid name is
        // never persisted for a later "on".
        if (name is not null && StudioStreamOutputValidation.ValidateNdiGraphicsKeyName(programName, nextName) is { } nameError)
            return ControlInvokeResult.Fail(nameError);
        if (on && StudioStreamOutputValidation.NdiGraphicsKeyRefusal(ndiEnabled, coreHasCapability, programName, nextName) is { } refusal)
            return ControlInvokeResult.Fail(refusal);
        apply(on, name is null ? null : nextName);
        return ControlInvokeResult.Success;
    }
}
