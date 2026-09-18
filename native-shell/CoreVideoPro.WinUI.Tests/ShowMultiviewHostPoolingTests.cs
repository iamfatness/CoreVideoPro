using System;
using System.IO;
using Xunit;

namespace CoreVideoPro.WinUI.Tests;

/// <summary>
/// Guard for #513: the two high-traffic multiview overlays (click buttons and the
/// per-tile decorations) must be POOLED — created once and updated in place — not
/// cleared and rebuilt. Clearing discards XAML elements to the finalizer, which is the
/// crash population this control dominates during a live show. (The transient
/// DragOverlay, two elements torn down at drag end, is exempt and not asserted here.)
/// Source-analysis test in the Page_GuardsEveryUiCallback shape.
/// </summary>
public sealed class ShowMultiviewHostPoolingTests
{
    [Fact]
    public void HotOverlaysAreNeverClearedAndRebuilt()
    {
        var code = ReadControl("ShowMultiviewHost.xaml.cs");

        Assert.DoesNotContain("ClickOverlay.Children.Clear", code, StringComparison.Ordinal);
        Assert.DoesNotContain("DecorOverlay.Children.Clear", code, StringComparison.Ordinal);

        // The pool must exist and be reused (Add happens only in the Ensure* helpers).
        Assert.Contains("_overlayButtons", code, StringComparison.Ordinal);
        Assert.Contains("_decorCells", code, StringComparison.Ordinal);
        Assert.Contains("EnsureOverlayButton", code, StringComparison.Ordinal);
        Assert.Contains("EnsureDecorCell", code, StringComparison.Ordinal);
    }

    private static string ReadControl(string fileName)
    {
        for (var directory = new DirectoryInfo(AppContext.BaseDirectory);
             directory is not null;
             directory = directory.Parent)
        {
            var candidate = Path.Combine(
                directory.FullName, "native-shell", "CoreVideoPro.WinUI", "Controls", fileName);
            if (File.Exists(candidate))
            {
                return File.ReadAllText(candidate);
            }
        }

        throw new FileNotFoundException($"Could not locate {fileName} from the test output directory.");
    }
}
