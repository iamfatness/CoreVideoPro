using Xunit;

namespace CoreVideoPro.WinUI.Tests;

public sealed class OutputLifecyclePollingTests
{
    [Fact]
    public void CaptureDisabledPollingStillAppliesOutputLifecycleOnUiThread()
    {
        // The full VM constructs WinUI windows and launches native services. Guard this
        // platform wiring here; lifecycle mapping and command behavior have runtime tests.
        var directory = new DirectoryInfo(AppContext.BaseDirectory);
        string? path = null;
        while (directory is not null)
        {
            var candidate = Path.Combine(directory.FullName, "native-shell", "CoreVideoPro.WinUI", "ViewModels", "StudioViewModel.cs");
            if (File.Exists(candidate)) { path = candidate; break; }
            directory = directory.Parent;
        }
        Assert.NotNull(path);
        var source = File.ReadAllText(path!);
        Assert.Contains("RunOnUiThread(() => ApplySnapshotChanged(snapshot))", source);
        var begin = source.IndexOf("private void ApplySnapshotChanged(", StringComparison.Ordinal);
        var gate = source.IndexOf("if (!ZoomCaptureSubscribed)", begin, StringComparison.Ordinal);
        var earlyReturn = source.IndexOf("return;", gate, StringComparison.Ordinal);
        var outputApply = source.IndexOf("ApplyOutputLifecyclePatch(", gate, StringComparison.Ordinal);
        Assert.InRange(outputApply, gate, earlyReturn);
        var transportSource = File.ReadAllText(Path.Combine(Path.GetDirectoryName(path!)!, "StudioViewModel.Transport.cs"));
        var helper = transportSource[transportSource.IndexOf("private void ApplyOutputLifecyclePatch(", StringComparison.Ordinal)..];
        Assert.Contains("Recording = recording;", helper);
        Assert.Contains("Streaming = streaming;", helper);
        Assert.Contains("RecordingRequested = false;", helper);
    }
}
