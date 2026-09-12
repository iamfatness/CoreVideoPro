using System.Text.RegularExpressions;
using Xunit;

namespace CoreVideoPro.WinUI.Tests;

/// <summary>
/// #480: the per-layer source ComboBox lives in an ItemsRepeater template.
/// SelectedValue must not be driven by x:Bind (CLAUDE.md live-QA crash class);
/// selection is applied on Loaded, guarded, and only an operator gesture may
/// commit a pick.
/// </summary>
public sealed class SourcesPageLayerSourceContentTests
{
    [Fact]
    public void LayerSourceComboDoesNotDriveSelectedValueByXBind()
    {
        var xaml = ReadView("SourcesPage.xaml");
        var combo = LayerSourceCombo(xaml);
        Assert.DoesNotContain("SelectedValue=", combo, StringComparison.Ordinal);
        Assert.Contains("Loaded=\"OnLayerSourceComboLoaded\"", combo, StringComparison.Ordinal);
        Assert.Contains("DropDownOpened=\"OnLayerSourceDropDownOpened\"", combo, StringComparison.Ordinal);
        Assert.Contains("DropDownClosed=\"OnLayerSourceDropDownClosed\"", combo, StringComparison.Ordinal);
        Assert.Contains("SelectionChanged=\"OnLayerSourceSelectionChanged\"", combo, StringComparison.Ordinal);
    }

    [Fact]
    public void LayerSourceHandlerCommitsOnlyOperatorGesturesAndLogsTheCause()
    {
        var code = ReadView("SourcesPage.xaml.cs");
        Assert.Contains("TryCommitSourceOption", code, StringComparison.Ordinal);
        Assert.Contains("BeginOperatorSourcePick", code, StringComparison.Ordinal);
        Assert.Contains("EndOperatorSourcePick", code, StringComparison.Ordinal);
        Assert.Contains("RestoreLayerSourceCombo", code, StringComparison.Ordinal);
        Assert.Contains("RefreshIgnoredCause", code, StringComparison.Ordinal);
        Assert.Contains("OperatorCause", code, StringComparison.Ordinal);
        Assert.DoesNotContain("TrySelectSourceOption(option)", code, StringComparison.Ordinal);
    }

    private static string LayerSourceCombo(string xaml)
    {
        var match = Regex.Match(
            xaml,
            @"<ComboBox Tag=""\{x:Bind\}""[^>]*SelectionChanged=""OnLayerSourceSelectionChanged""[^>]*/?>",
            RegexOptions.Singleline);
        Assert.True(match.Success, "layer source ComboBox not found");
        return match.Value;
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
                return File.ReadAllText(candidate);
        }

        throw new FileNotFoundException($"Could not locate {fileName} from the test output directory.");
    }
}
