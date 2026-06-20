using CoreVideoPro.WinUI.Services;
using Xunit;

namespace CoreVideoPro.WinUI.Tests;

public sealed class StudioLayoutPreferenceServiceTests
{
    [Fact]
    public void NormalizeClampsRatiosToOperatorSafeRange()
    {
        var preferences = StudioLayoutPreferenceService.Normalize(new StudioLayoutPreferences
        {
            PreviewProgramSplitRatio = 0.95,
            ProgramPreviewHeightRatio = 0.1
        });

        Assert.Equal(StudioLayoutPreferenceService.MaxPreviewProgramSplitRatio, preferences.PreviewProgramSplitRatio);
        Assert.Equal(StudioLayoutPreferenceService.MinProgramPreviewHeightRatio, preferences.ProgramPreviewHeightRatio);
    }

    [Fact]
    public void SaveAndLoadRoundTripsLayoutPreferences()
    {
        var path = Path.Combine(Path.GetTempPath(), $"corevideo-layout-{Guid.NewGuid():N}.json");
        var service = new StudioLayoutPreferenceService(path);
        var expected = new StudioLayoutPreferences
        {
            PreviewProgramSplitRatio = 0.62,
            ProgramPreviewHeightRatio = 0.71
        };

        service.Save(expected);

        var loaded = service.Load();
        Assert.Equal(expected.PreviewProgramSplitRatio, loaded.PreviewProgramSplitRatio);
        Assert.Equal(expected.ProgramPreviewHeightRatio, loaded.ProgramPreviewHeightRatio);

        File.Delete(path);
    }
}
