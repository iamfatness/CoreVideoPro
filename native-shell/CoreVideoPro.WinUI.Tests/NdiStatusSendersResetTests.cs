using Xunit;

namespace CoreVideoPro.WinUI.Tests;

/// <summary>
/// Final review: the NDI tile reads a cache of the last snapshot's senders. A stopped or
/// recovering core sends no more snapshots, so the bridge-health handler must drop that cache or
/// the tile keeps saying `Live · name` for a dead core. StudioViewModel cannot be constructed in
/// a unit test, so this pins the source the way TransportCoordinatorTests does.
/// </summary>
public sealed class NdiStatusSendersResetTests
{
    [Fact]
    public void BridgeStopOrRespawnForgetsTheNdiSenders()
    {
        var studio = File.ReadAllText(FindRepoFile(Path.Combine(
            "native-shell", "CoreVideoPro.WinUI", "ViewModels", "StudioViewModel.cs")));

        var handler = Body(studio, "private void ApplyBridgeHealthChanged(MediaCoreHealth health)", "Settings.RefreshDiagnosticsReadout();");
        Assert.Contains("if (health.Stopped || health.Recovering)", handler);
        Assert.Contains("ForgetNdiStatusSenders();", handler);

        var forget = Body(studio, "private void ForgetNdiStatusSenders()", "}");
        Assert.Contains("_ndiStatusSenders = [];", forget);
        Assert.Contains("RefreshNdiOutputStatusLabel();", forget);
    }

    private static string Body(string source, string declaration, string endMarker)
    {
        var start = source.IndexOf(declaration, StringComparison.Ordinal);
        Assert.True(start >= 0, $"'{declaration}' not found");
        var end = source.IndexOf(endMarker, start, StringComparison.Ordinal);
        Assert.True(end >= 0, $"'{endMarker}' not found after '{declaration}'");
        return source[start..end];
    }

    private static string FindRepoFile(string relativePath)
    {
        for (var dir = new DirectoryInfo(AppContext.BaseDirectory); dir is not null; dir = dir.Parent)
        {
            var candidate = Path.Combine(dir.FullName, relativePath);
            if (File.Exists(candidate))
            {
                return candidate;
            }
        }

        throw new FileNotFoundException($"Could not locate {relativePath} above {AppContext.BaseDirectory}.");
    }
}
