using CoreVideoPro.MediaCore.Models;
using CoreVideoPro.WinUI.ViewModels;
using Xunit;

namespace CoreVideoPro.WinUI.Tests;

// A1 (VST round-2): editor/respawn state must reach the operator as words —
// the pre-fix defect was exactly a swallowed failure ("Open controls" did
// nothing, ever). These pin the operator-facing mappings.
public sealed class VstEditorStatusTests
{
    [Fact]
    public void FormatVstEditorStatus_IdleIsEmpty()
    {
        Assert.Equal(string.Empty, StudioViewModel.FormatVstEditorStatus(new NativeMediaCorePluginHostServe()));
    }

    [Fact]
    public void FormatVstEditorStatus_OpenNamesThePlugin()
    {
        var status = StudioViewModel.FormatVstEditorStatus(new NativeMediaCorePluginHostServe
        {
            EditorStatusCode = 1,
            EditorActivePlugin = "Curves AQ Stereo",
        });
        Assert.Equal("Controls open: Curves AQ Stereo.", status);
    }

    [Fact]
    public void FormatVstEditorStatus_CreateViewNullSaysNoEditor()
    {
        // The host reports createView(null) as this exact error string; the
        // spec wording for the chip is "This plugin has no editor."
        var status = StudioViewModel.FormatVstEditorStatus(new NativeMediaCorePluginHostServe
        {
            EditorStatusCode = 2,
            EditorActivePlugin = "Curves AQ Stereo",
            EditorLastError = "plugin does not provide a native editor",
        });
        Assert.Equal("This plugin has no editor.", status);
    }

    [Fact]
    public void FormatVstEditorStatus_OtherFailuresCarryTheHostError()
    {
        var status = StudioViewModel.FormatVstEditorStatus(new NativeMediaCorePluginHostServe
        {
            EditorStatusCode = 2,
            EditorActivePlugin = "Curves AQ Stereo",
            EditorLastError = "plugin editor failed to attach",
        });
        Assert.Equal("Controls failed: plugin editor failed to attach", status);
    }

    [Fact]
    public void FormatVstServeStatus_GiveUpOutranksEverything()
    {
        var status = StudioViewModel.FormatVstServeStatus(new NativeMediaCorePluginHostServe
        {
            Running = true,
            StatusCode = 1,
            Exchanges = 100,
            ActivePlugin = "Curves AQ Stereo",
            Respawn = new NativeMediaCorePluginHostRespawn { Attempts = 5, GaveUp = true },
        });
        Assert.Contains("crashed repeatedly", status);
        Assert.Contains("bypassed", status);
        Assert.Contains("Re-select", status);
    }

    [Fact]
    public void FormatVstServeStatus_AppendsEditorStateToTheAudioState()
    {
        var status = StudioViewModel.FormatVstServeStatus(new NativeMediaCorePluginHostServe
        {
            Running = true,
            StatusCode = 1,
            Exchanges = 100,
            ActivePlugin = "Curves AQ Stereo",
            EditorStatusCode = 1,
            EditorActivePlugin = "Curves AQ Stereo",
        });
        Assert.Equal(
            "Processing Curves AQ Stereo in the isolated VST3 host. Controls open: Curves AQ Stereo.",
            status);
    }

    [Theory]
    [InlineData("VST: Curves AQ Stereo", "Curves AQ Stereo", true)]
    [InlineData("vst:WaveShell1-VST3 15.11/Curves AQ Stereo", "Curves AQ Stereo", true)]
    [InlineData("VST: Curves AQ Stereo", "curves aq stereo", true)]
    [InlineData("VST: Some Other Plugin", "Curves AQ Stereo", false)]
    [InlineData("VST: Curves AQ Stereo", "", false)]
    public void InsertMatchesVstPlugin_MatchesLikeTheProcessingBadge(
        string insertName, string pluginName, bool expected)
    {
        Assert.Equal(expected, StudioViewModel.InsertMatchesVstPlugin(insertName, pluginName));
    }
}
