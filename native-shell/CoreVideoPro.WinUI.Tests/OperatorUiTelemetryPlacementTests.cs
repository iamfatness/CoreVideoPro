using Xunit;

namespace CoreVideoPro.WinUI.Tests;

public sealed class OperatorUiTelemetryPlacementTests
{
    [Fact]
    public void ZoomPage_KeepsLiveEngineEvidenceInDedicatedHealthWindow()
    {
        var xaml = ReadView("SettingsPage.xaml");

        Assert.Contains("Zoom readiness", xaml, StringComparison.Ordinal);
        Assert.DoesNotContain("ZoomEngineEvidence", xaml, StringComparison.Ordinal);
        Assert.DoesNotContain("DiagnosticsReadout", xaml, StringComparison.Ordinal);
        Assert.DoesNotContain("Zoom engine evidence", xaml, StringComparison.Ordinal);
        Assert.DoesNotContain("Diagnostics &amp; support", xaml, StringComparison.Ordinal);
    }

    [Fact]
    public void Studio_KeepsDeveloperCountersOutOfPersistentChrome()
    {
        var xaml = ReadView("StudioWorkspace.xaml");

        Assert.DoesNotContain("Transport.CpuLoadLine", xaml, StringComparison.Ordinal);
        Assert.DoesNotContain("Transport.MemoryLoadLine", xaml, StringComparison.Ordinal);
        Assert.DoesNotContain("Transport.DiskLoadLine", xaml, StringComparison.Ordinal);
        Assert.DoesNotContain("ViewModel.NativeLowerThirdStatus", xaml, StringComparison.Ordinal);
    }

    [Fact]
    public void AudioAndOutputSettings_UseOperatorStatusInsteadOfEngineCounters()
    {
        var audioXaml = ReadView("AudioPage.xaml");
        var outputXaml = ReadView("ProductionSettingsWindow.xaml");

        Assert.DoesNotContain("Header=\"Diagnostics\"", audioXaml, StringComparison.Ordinal);
        Assert.DoesNotContain("ViewModel.AudioMonitorEngineStatus", audioXaml, StringComparison.Ordinal);
        Assert.DoesNotContain("ViewModel.AudioMonitorEngineStatus", outputXaml, StringComparison.Ordinal);
        Assert.DoesNotContain("COREVIDEO_FFMPEG_BIN_DIR", outputXaml, StringComparison.Ordinal);
        Assert.DoesNotContain("FFMPEG_BIN_DIR", outputXaml, StringComparison.Ordinal);
    }

    private static string ReadView(string fileName)
    {
        for (var directory = new DirectoryInfo(AppContext.BaseDirectory);
             directory is not null;
             directory = directory.Parent)
        {
            var candidate = Path.Combine(
                directory.FullName,
                "native-shell",
                "CoreVideoPro.WinUI",
                "Views",
                fileName);
            if (File.Exists(candidate))
            {
                return File.ReadAllText(candidate);
            }
        }

        throw new FileNotFoundException($"Could not locate {fileName} from the test output directory.");
    }
}
