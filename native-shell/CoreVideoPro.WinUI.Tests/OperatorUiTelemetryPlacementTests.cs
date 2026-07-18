using Xunit;

namespace CoreVideoPro.WinUI.Tests;

public sealed class OperatorUiTelemetryPlacementTests
{
    [Fact]
    public void ZoomPage_UsesOperatorStatusAndKeepsTechnicalEvidenceInHealth()
    {
        var xaml = ReadView("SettingsPage.xaml");

        Assert.Contains("Zoom status", xaml, StringComparison.Ordinal);
        Assert.Contains("ViewModel.ZoomStatusGuidance", xaml, StringComparison.Ordinal);
        Assert.DoesNotContain("Zoom readiness", xaml, StringComparison.Ordinal);
        Assert.DoesNotContain("ViewModel.SdkBlockers", xaml, StringComparison.Ordinal);
        Assert.DoesNotContain("Public Client OAuth", xaml, StringComparison.Ordinal);
        Assert.DoesNotContain("Detailed engine information", xaml, StringComparison.Ordinal);
        Assert.DoesNotContain("ZoomEngineEvidence", xaml, StringComparison.Ordinal);
        Assert.DoesNotContain("DiagnosticsReadout", xaml, StringComparison.Ordinal);
        Assert.DoesNotContain("Zoom engine evidence", xaml, StringComparison.Ordinal);
        Assert.DoesNotContain("Diagnostics &amp; support", xaml, StringComparison.Ordinal);
        Assert.DoesNotContain("SdkChipWarning", xaml, StringComparison.Ordinal);
        Assert.DoesNotContain("ViewModel.SdkIsWarning", xaml, StringComparison.Ordinal);
        Assert.Contains("ViewModel.ZoomOperatorIsReady", xaml, StringComparison.Ordinal);
        Assert.Contains("ViewModel.ZoomOperatorIsBlocked", xaml, StringComparison.Ordinal);
    }

    [Fact]
    public void Studio_KeepsDeveloperCountersOutOfPersistentChrome()
    {
        var xaml = ReadView("StudioWorkspace.xaml");

        Assert.DoesNotContain("Transport.CpuLoadLine", xaml, StringComparison.Ordinal);
        Assert.DoesNotContain("Transport.MemoryLoadLine", xaml, StringComparison.Ordinal);
        Assert.DoesNotContain("Transport.DiskLoadLine", xaml, StringComparison.Ordinal);
        Assert.DoesNotContain("ViewModel.NativeLowerThirdStatus", xaml, StringComparison.Ordinal);
        Assert.DoesNotContain("Show readiness", xaml, StringComparison.Ordinal);
        Assert.DoesNotContain("ViewModel.ShowReadinessSummary", xaml, StringComparison.Ordinal);
        Assert.DoesNotContain("ViewModel.SceneRailDisplaySummary", xaml, StringComparison.Ordinal);
        Assert.DoesNotContain("ViewModel.SceneBuilderHint", xaml, StringComparison.Ordinal);
        Assert.DoesNotContain("Tap a scene to queue", xaml, StringComparison.Ordinal);
        Assert.DoesNotContain("ViewModel.AudioMonitorEngineStatus", xaml, StringComparison.Ordinal);
        Assert.DoesNotContain("ViewModel.OutputStatus,", xaml, StringComparison.Ordinal);
        Assert.DoesNotContain("crosspoint matrix", xaml, StringComparison.OrdinalIgnoreCase);
        Assert.Contains("ViewModel.OutputOperatorStatus", xaml, StringComparison.Ordinal);
    }

    [Fact]
    public void AudioAndOutputSettings_UseOperatorStatusInsteadOfEngineCounters()
    {
        var audioXaml = ReadView("AudioPage.xaml");
        var outputXaml = ReadView("ProductionSettingsWindow.xaml");

        Assert.DoesNotContain("Header=\"Diagnostics\"", audioXaml, StringComparison.Ordinal);
        Assert.DoesNotContain("ViewModel.AudioMonitorEngineStatus", audioXaml, StringComparison.Ordinal);
        Assert.DoesNotContain("ViewModel.LocalAudioSourceStatus", audioXaml, StringComparison.Ordinal);
        Assert.DoesNotContain("ViewModel.LocalAudioSourceRecommendation", audioXaml, StringComparison.Ordinal);
        Assert.DoesNotContain("ViewModel.AudioMonitorEngineStatus", outputXaml, StringComparison.Ordinal);
        Assert.DoesNotContain("ViewModel.LocalAudioSourceStatus", outputXaml, StringComparison.Ordinal);
        Assert.DoesNotContain("ViewModel.LocalAudioSourceRecommendation", outputXaml, StringComparison.Ordinal);
        Assert.DoesNotContain("COREVIDEO_FFMPEG_BIN_DIR", outputXaml, StringComparison.Ordinal);
        Assert.DoesNotContain("FFMPEG_BIN_DIR", outputXaml, StringComparison.Ordinal);
        Assert.DoesNotContain("ViewModel.FfmpegRuntimeStatus", outputXaml, StringComparison.Ordinal);
        Assert.DoesNotContain("staged beside the packaged app", outputXaml, StringComparison.OrdinalIgnoreCase);
        Assert.DoesNotContain("local alpha toggles", outputXaml, StringComparison.OrdinalIgnoreCase);
        Assert.DoesNotContain("licensing backend", outputXaml, StringComparison.OrdinalIgnoreCase);
        Assert.DoesNotContain("Plan upgrades", outputXaml, StringComparison.Ordinal);
        Assert.DoesNotContain("crosspoint matrix", audioXaml, StringComparison.OrdinalIgnoreCase);
        Assert.Contains("ViewModel.FfmpegOperatorStatus", outputXaml, StringComparison.Ordinal);
    }

    [Fact]
    public void RoutingPage_UsesOperatorLanguage()
    {
        var xaml = ReadView("RoutingPage.xaml");

        Assert.Contains("Click a cell to send a source", xaml, StringComparison.Ordinal);
        Assert.DoesNotContain("Video crosspoints", xaml, StringComparison.OrdinalIgnoreCase);
    }

    [Fact]
    public void MasterBusProcessing_UsesAnOrderedAlwaysVisibleSignalChain()
    {
        var xaml = ReadView("AudioPage.xaml");

        Assert.Contains("Master processor", xaml, StringComparison.Ordinal);
        Assert.Contains("01  INPUT", xaml, StringComparison.Ordinal);
        Assert.Contains("06  SAFETY LIMITER", xaml, StringComparison.Ordinal);
        Assert.Contains("Program L/R · processing order", xaml, StringComparison.Ordinal);
        Assert.DoesNotContain("Native DSP", xaml, StringComparison.Ordinal);
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
        Assert.DoesNotContain("ViewModel.ProcessingBridgeStatusLabel", xaml, StringComparison.Ordinal);
        Assert.DoesNotContain("isolated host", xaml, StringComparison.OrdinalIgnoreCase);
        Assert.DoesNotContain("fail-open", xaml, StringComparison.OrdinalIgnoreCase);
    }

    [Fact]
    public void AudioPage_OffersValidatedVstPluginsWithoutPlaceholderLanguage()
    {
        var xaml = ReadView("AudioPage.xaml");
        var codeBehind = ReadView("AudioPage.xaml.cs");

        Assert.Contains("Add VST3 plug-in", xaml, StringComparison.Ordinal);
        Assert.Contains("viewModel.FilteredVstPlugins", codeBehind, StringComparison.Ordinal);
        Assert.DoesNotContain("isolated host", codeBehind, StringComparison.OrdinalIgnoreCase);
        Assert.DoesNotContain("fail-open", codeBehind, StringComparison.OrdinalIgnoreCase);
        Assert.DoesNotContain("Add VST3 slot", xaml, StringComparison.Ordinal);
        Assert.DoesNotContain("pass-through until live hosting ships", codeBehind, StringComparison.OrdinalIgnoreCase);
    }

    [Fact]
    public void LowerThirdDesigner_ExposesRealTakeRebuildAndTimingControls()
    {
        var xaml = ReadView("OverlaysPage.xaml");

        Assert.Contains("ViewModel.ToggleProgramLowerThirdCommand", xaml, StringComparison.Ordinal);
        Assert.Contains("ViewModel.RebuildProgramLowerThirdCommand", xaml, StringComparison.Ordinal);
        Assert.Contains("ViewModel.LowerThirdTimingPresetOptions", xaml, StringComparison.Ordinal);
        Assert.Contains("SOURCE BEHAVIOR", xaml, StringComparison.Ordinal);
        Assert.Contains("stays locked while that source remains on program", xaml, StringComparison.Ordinal);
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
