using System.Text.Json;

namespace CoreVideoPro.WinUI.Services;

public sealed record StudioLayoutPreferences
{
    public double PreviewProgramSplitRatio { get; init; } = 0.5;

    public double ProgramPreviewHeightRatio { get; init; } = 0.67;
}

public sealed class StudioLayoutPreferenceService
{
    public const double MinPreviewProgramSplitRatio = 0.25;
    public const double MaxPreviewProgramSplitRatio = 0.75;
    public const double MinProgramPreviewHeightRatio = 0.45;
    public const double MaxProgramPreviewHeightRatio = 0.82;

    private readonly string _preferencePath;

    public StudioLayoutPreferenceService()
        : this(Path.Combine(
            Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData),
            "CoreVideoPro",
            "studio-layout.json"))
    {
    }

    public StudioLayoutPreferenceService(string preferencePath)
    {
        _preferencePath = preferencePath;
    }

    public StudioLayoutPreferences Load()
    {
        try
        {
            if (!File.Exists(_preferencePath))
            {
                return new StudioLayoutPreferences();
            }

            var preferences = JsonSerializer.Deserialize<StudioLayoutPreferences>(
                File.ReadAllText(_preferencePath));

            return Normalize(preferences ?? new StudioLayoutPreferences());
        }
        catch
        {
            return new StudioLayoutPreferences();
        }
    }

    public void Save(StudioLayoutPreferences preferences)
    {
        try
        {
            var directory = Path.GetDirectoryName(_preferencePath);
            if (!string.IsNullOrWhiteSpace(directory))
            {
                Directory.CreateDirectory(directory);
            }

            File.WriteAllText(
                _preferencePath,
                JsonSerializer.Serialize(Normalize(preferences), new JsonSerializerOptions { WriteIndented = true }));
        }
        catch
        {
            // Layout preferences should never block production operation.
        }
    }

    public static StudioLayoutPreferences Normalize(StudioLayoutPreferences preferences) =>
        preferences with
        {
            PreviewProgramSplitRatio = ClampFinite(
                preferences.PreviewProgramSplitRatio,
                MinPreviewProgramSplitRatio,
                MaxPreviewProgramSplitRatio,
                0.5),
            ProgramPreviewHeightRatio = ClampFinite(
                preferences.ProgramPreviewHeightRatio,
                MinProgramPreviewHeightRatio,
                MaxProgramPreviewHeightRatio,
                0.67)
        };

    private static double ClampFinite(double value, double min, double max, double fallback) =>
        double.IsFinite(value) ? Math.Clamp(value, min, max) : fallback;
}
