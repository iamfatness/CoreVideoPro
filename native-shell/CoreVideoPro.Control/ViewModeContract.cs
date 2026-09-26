namespace CoreVideoPro.Control;

/// <summary>The accepted Control API view modes and the legacy dual-view alias.</summary>
public static class ViewModeContract
{
    public const string SupportedModes = "program, preview, program-preview, multiview";

    public static bool TryNormalize(string? mode, out string canonical)
    {
        canonical = mode switch
        {
            "program" or "preview" or "program-preview" or "multiview" => mode,
            "programPreview" => "program-preview",
            _ => ""
        };
        return canonical.Length != 0;
    }
}
