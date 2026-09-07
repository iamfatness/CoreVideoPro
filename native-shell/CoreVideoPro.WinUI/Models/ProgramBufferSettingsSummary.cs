using CoreVideoPro.MediaCore.Models;

namespace CoreVideoPro.WinUI.Models;

public static class ProgramBufferSettingsSummary
{
    public static bool RequiresRestart(int startupFrames, int selectedFrames) =>
        ProgramBufferPreference.Normalize(startupFrames) != ProgramBufferPreference.Normalize(selectedFrames);

    public static string Describe(int startupFrames, int selectedFrames)
    {
        var startup = ProgramBufferPreference.Normalize(startupFrames);
        var selected = ProgramBufferPreference.Normalize(selectedFrames);
        var session = $"This app session: {startup} frames requested.";
        return RequiresRestart(startup, selected)
            ? $"{session} Restart the app to apply {selected} frames."
            : $"{session} No restart required.";
    }
}
