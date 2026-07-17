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
        Assert.DoesNotContain("ViewModel.LocalAudioSourceStatus", audioXaml, StringComparison.Ordinal);
        Assert.DoesNotContain("ViewModel.AudioMonitorEngineStatus", outputXaml, StringComparison.Ordinal);
        Assert.DoesNotContain("ViewModel.LocalAudioSourceStatus", outputXaml, StringComparison.Ordinal);
        Assert.DoesNotContain("COREVIDEO_FFMPEG_BIN_DIR", outputXaml, StringComparison.Ordinal);
        Assert.DoesNotContain("FFMPEG_BIN_DIR", outputXaml, StringComparison.Ordinal);
    }

    [Fact]
    public void MasterBusProcessing_UsesAnOrderedAlwaysVisibleSignalChain()
    {
        var xaml = ReadView("AudioPage.xaml");

        Assert.Contains("Master processor", xaml, StringComparison.Ordinal);
        Assert.Contains("01  INPUT", xaml, StringComparison.Ordinal);
        Assert.Contains("06  SAFETY LIMITER", xaml, StringComparison.Ordinal);
        Assert.Contains("Native DSP · Program L/R · ordered pre-output chain", xaml, StringComparison.Ordinal);
        Assert.Contains("controls:MasteringToneCurve", xaml, StringComparison.Ordinal);
        Assert.Contains("ViewModel.SelectMasteringCompareSlotCommand", xaml, StringComparison.Ordinal);
        Assert.Contains("ViewModel.ApplyMasteringPresetCommand", xaml, StringComparison.Ordinal);
        Assert.Contains("ViewModel.ResetMasteringStageCommand", xaml, StringComparison.Ordinal);
        Assert.Contains("A/B stores two complete processor states", xaml, StringComparison.Ordinal);
        Assert.DoesNotContain("ViewModel.AudioValidationFirstIssue", xaml, StringComparison.Ordinal);
        Assert.DoesNotContain("Header=\"Mastering rack\"", xaml, StringComparison.Ordinal);
        Assert.DoesNotContain("land next", xaml, StringComparison.OrdinalIgnoreCase);
        Assert.Contains("Advanced · VST3 plug-in browser", xaml, StringComparison.Ordinal);
        Assert.Contains("IsExpanded=\"False\"", xaml, StringComparison.Ordinal);
        Assert.Contains("SIGNAL FLOW · TOP TO BOTTOM", xaml, StringComparison.Ordinal);
        Assert.Contains("OnProcessingSlotClicked", xaml, StringComparison.Ordinal);
        Assert.Contains("OnRemoveProcessingSlotClicked", xaml, StringComparison.Ordinal);
        Assert.Contains("Plug-in failure always bypasses safely", xaml, StringComparison.Ordinal);
    }

    [Fact]
    public void AudioPage_OffersValidatedVstPluginsWithoutPlaceholderLanguage()
    {
        var xaml = ReadView("AudioPage.xaml");
        var codeBehind = ReadView("AudioPage.xaml.cs");

        Assert.Contains("Add VST3 plug-in", xaml, StringComparison.Ordinal);
        Assert.Contains("isolated host", codeBehind, StringComparison.OrdinalIgnoreCase);
        Assert.Contains("viewModel.FilteredVstPlugins", codeBehind, StringComparison.Ordinal);
        Assert.DoesNotContain("Add VST3 slot", xaml, StringComparison.Ordinal);
        Assert.DoesNotContain("pass-through until live hosting ships", codeBehind, StringComparison.OrdinalIgnoreCase);
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
