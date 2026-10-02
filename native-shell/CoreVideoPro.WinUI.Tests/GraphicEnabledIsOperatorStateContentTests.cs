using System.Text.RegularExpressions;
using Xunit;

namespace CoreVideoPro.WinUI.Tests;

/// <summary>
/// #757: a graphic's <c>Enabled</c> flag is the operator's desired state. A snapshot
/// handler used to overwrite it from the core's render-plan layer list, which a real
/// core never publishes, so any overlay on air switched every graphic off again and
/// the next sync took it off Program. StudioViewModel is not constructible in tests,
/// so this pins the rule on the source: no method that takes a snapshot may assign it.
/// </summary>
public sealed class GraphicEnabledIsOperatorStateContentTests
{
    private static readonly Regex MethodHeader = new(
        @"^    (?:private|public|internal|protected)[^=;\n]*\(", RegexOptions.Multiline);

    [Fact]
    public void NoSnapshotHandlerAssignsAGraphicsEnabledFlag()
    {
        var offenders = new List<string>();
        var assignments = 0;
        foreach (var file in ViewModelSources())
        {
            var code = File.ReadAllText(file);
            var headers = MethodHeader.Matches(code);
            for (var index = 0; index < headers.Count; index++)
            {
                var start = headers[index].Index;
                var end = index + 1 < headers.Count ? headers[index + 1].Index : code.Length;
                var method = code[start..end];
                if (!method.Contains("graphic.Enabled =", StringComparison.Ordinal))
                {
                    continue;
                }

                assignments++;
                var header = method[..method.IndexOf('\n')];
                if (header.Contains("Snapshot", StringComparison.Ordinal))
                {
                    offenders.Add($"{Path.GetFileName(file)}: {header.Trim()}");
                }
            }
        }

        Assert.True(assignments > 0, "the scan found no graphic.Enabled assignment at all; it is looking in the wrong place");
        Assert.Empty(offenders);
    }

    [Fact]
    public void TheRenderPlanOverlayWriteBackIsGone()
    {
        foreach (var file in ViewModelSources())
        {
            var code = File.ReadAllText(file);
            Assert.DoesNotContain("ResolveOverlayEnabledFlags", code, StringComparison.Ordinal);
            Assert.DoesNotContain("ApplyOverlayStateFromSnapshot", code, StringComparison.Ordinal);
        }
    }

    private static IEnumerable<string> ViewModelSources()
    {
        for (var directory = new DirectoryInfo(AppContext.BaseDirectory);
             directory is not null;
             directory = directory.Parent)
        {
            var candidate = Path.Combine(directory.FullName, "native-shell", "CoreVideoPro.WinUI", "ViewModels");
            if (Directory.Exists(candidate))
            {
                return Directory.EnumerateFiles(candidate, "StudioViewModel*.cs", SearchOption.TopDirectoryOnly).ToList();
            }
        }

        throw new DirectoryNotFoundException("native-shell/CoreVideoPro.WinUI/ViewModels");
    }
}
