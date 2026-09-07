namespace CoreVideoPro.WinUI.Services;

/// <summary>Startup-only operator notices; normal first launch stays quiet.</summary>
public static class ProductionPreferencesNotice
{
    public const string RestoreFailure =
        "Saved scenes and output settings could not be fully restored. Review the studio settings before going live.";

    public static string For(ProductionPreferencesLoadStatus status) => status switch
    {
        ProductionPreferencesLoadStatus.Recovered =>
            "Scenes and output settings were recovered from a backup. Recent changes may be missing. Review the studio settings before going live.",
        ProductionPreferencesLoadStatus.Corrupt =>
            "Saved scenes and output settings are damaged, and no usable backup was found. Default settings were loaded. Review the studio settings before going live.",
        ProductionPreferencesLoadStatus.Unreadable =>
            "CoreVideo Pro could not access the saved scenes and output settings. Default settings were loaded. Check file access and review the studio settings before going live.",
        _ => string.Empty
    };
}
