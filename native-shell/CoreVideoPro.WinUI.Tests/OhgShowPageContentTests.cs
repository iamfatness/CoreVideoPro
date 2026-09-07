using System.Text.RegularExpressions;
using CoreVideoPro.WinUI.Models;
using CoreVideoPro.WinUI.ViewModels;
using CoreVideoPro.WinUI.Views;
using Xunit;

namespace CoreVideoPro.WinUI.Tests;

/// <summary>
/// Plan 7b Task 7 — the OHG Show tab. These are XAML-TEXT assertions (the
/// <see cref="OperatorUiTelemetryPlacementTests"/> pattern): the page is compiled by the WinUI
/// build, not by this test project, so the only thing a unit test can hold is the page's CONTRACT
/// with the view model and the house rules that have historically killed the app.
///
/// The two rules worth a test of their own:
/// <list type="bullet">
/// <item><b>No <c>Selector.SelectedValue</c>/<c>SelectedItem</c> driven by x:Bind.</b> CLAUDE.md,
/// live-QA day: a recycled ItemsRepeater container re-bound while its ItemsSource was still
/// resolving threw COMException 0x80004005 out of <c>Selector.set_SelectedValue</c> and killed the
/// app. Selection is applied in code-behind, guarded.</item>
/// <item><b>Every Button and ComboBox carries an <c>AutomationProperties.Name</c>.</b> Same file:
/// the unnamed transport buttons were invisible to screen readers AND to our own UIA tooling.</item>
/// </list>
/// </summary>
public sealed class OhgShowPageContentTests
{
    [Fact]
    public void Page_MergesOperatorTabResources()
    {
        var xaml = ReadView("OhgShowPage.xaml");

        Assert.Contains("ms-appx:///Views/OperatorTabResources.xaml", xaml, StringComparison.Ordinal);
    }

    [Fact]
    public void Page_BindsTheStatusStripAndTheSetupSurface()
    {
        var xaml = ReadView("OhgShowPage.xaml");

        Assert.Contains("ViewModel.OhgShow.EngineState", xaml, StringComparison.Ordinal);
        Assert.Contains("ViewModel.OhgShow.EngineDetail", xaml, StringComparison.Ordinal);
        Assert.Contains("ViewModel.OhgShow.IsShadowMode", xaml, StringComparison.Ordinal);
        Assert.Contains("ViewModel.OhgShow.ShadowLastCommand", xaml, StringComparison.Ordinal);
        Assert.Contains("ViewModel.OhgShow.PagingRefused", xaml, StringComparison.Ordinal);
        Assert.Contains("ViewModel.OhgShow.RestoreWarnings", xaml, StringComparison.Ordinal);
        Assert.Contains("ViewModel.OhgShow.RecentRefusals", xaml, StringComparison.Ordinal);
        Assert.Contains("ViewModel.OhgShow.LastActionStatus", xaml, StringComparison.Ordinal);
        Assert.Contains("ViewModel.OhgShow.RestartEngineCommand", xaml, StringComparison.Ordinal);

        // The null-VM case is a first-class surface, not a blank page.
        Assert.Contains("VisibleWhenNull(ViewModel.OhgShow)", xaml, StringComparison.Ordinal);
        Assert.Contains("VisibleWhenNotNull(ViewModel.OhgShow)", xaml, StringComparison.Ordinal);
        Assert.Contains("ViewModel.OpenOhgSettingsCommand", xaml, StringComparison.Ordinal);
    }

    [Fact]
    public void Page_BindsThePanelistBoard()
    {
        var xaml = ReadView("OhgShowPage.xaml");

        Assert.Contains("ViewModel.OhgShow.Panelists", xaml, StringComparison.Ordinal);
        Assert.Contains("ViewModel.OhgShow.Slots", xaml, StringComparison.Ordinal);
        Assert.Contains("ViewModel.OhgShow.Unseated", xaml, StringComparison.Ordinal);
        Assert.Contains("ViewModel.OhgShow.AssignSelectedToSlotCommand", xaml, StringComparison.Ordinal);
        Assert.Contains("ViewModel.OhgShow.AddSelectedToFirstEmptyCommand", xaml, StringComparison.Ordinal);
        Assert.Contains("ViewModel.OhgShow.RemoveSlotCommand", xaml, StringComparison.Ordinal);
        Assert.Contains("ViewModel.OhgShow.SyncAllCommand", xaml, StringComparison.Ordinal);

        // Roles is NOT bound in XAML on purpose: code-behind is the only writer of the role
        // ComboBox's ItemsSource, so an ElementName binding cannot resolve late and wipe the
        // selection this page applies by hand.
        var code = ReadView("OhgShowPage.xaml.cs");
        Assert.Contains("show.Roles", code, StringComparison.Ordinal);
        foreach (Match combo in Regex.Matches(xaml, @"<ComboBox(?=[\s/>])[^>]*>", RegexOptions.Singleline))
        {
            Assert.DoesNotContain("ItemsSource", combo.Value, StringComparison.Ordinal);
        }
    }

    /// <summary>
    /// A throwing UI callback fail-fasts the process with NO managed log (CLAUDE.md, UiDispatch).
    /// Every handler this page hooks from XAML must therefore be guarded; the guard bodies
    /// themselves have no test seam, so this asserts the SHAPE — every handler named in the XAML
    /// exists in the code-behind, and the file carries the guard plus its log call.
    /// </summary>
    [Fact]
    public void Page_GuardsEveryUiCallback()
    {
        var xaml = ReadView("OhgShowPage.xaml");
        var code = ReadView("OhgShowPage.xaml.cs");

        var handlers = Regex.Matches(xaml, @"(?:Click|Loaded|SelectionChanged|ElementPrepared|Tapped)=""(\w+)""")
            .Select(match => match.Groups[1].Value)
            .Distinct()
            .ToList();

        Assert.NotEmpty(handlers);
        foreach (var handler in handlers)
        {
            Assert.Contains(handler, code, StringComparison.Ordinal);
        }

        Assert.Contains("LaunchLog.Write", code, StringComparison.Ordinal);
        Assert.Contains("catch (Exception ex)", code, StringComparison.Ordinal);

        // Guarded(...) wraps the three handlers that are not already inside their own try/catch;
        // OnRoleComboLoaded delegates to SyncRoleCombo, which is.
        Assert.Contains("private void Guarded(", code, StringComparison.Ordinal);
        Assert.Contains("Guarded(\"panelist select\"", code, StringComparison.Ordinal);
        Assert.Contains("Guarded(\"seat select\"", code, StringComparison.Ordinal);
        Assert.Contains("Guarded(\"role change\"", code, StringComparison.Ordinal);
        Assert.Contains("Guarded(\"role combo realize\"", code, StringComparison.Ordinal);
    }

    // ── the one decision a guarded handler makes, extracted and tested ──

    [Fact]
    public void RoleChangeFor_SendsThePickedRoleWithThePin()
        => Assert.Equal(("1234", "host"), OhgShowPageLogic.RoleChangeFor("panelist", "1234", "host"));

    [Fact]
    public void RoleChangeFor_SendsAnEmptyPinSoTheViewModelCanRefuseItOutLoud()
        => Assert.Equal((string.Empty, "host"), OhgShowPageLogic.RoleChangeFor("panelist", null, "host"));

    [Theory]
    [InlineData("panelist", "panelist")]   // re-selecting the current role is not an edit
    [InlineData("panelist", null)]         // a ComboBox mid-rebind reports no selection
    [InlineData("panelist", "")]
    [InlineData("panelist", "   ")]
    public void RoleChangeFor_SendsNothingWhenThereIsNoRealChange(string current, string? picked)
        => Assert.Null(OhgShowPageLogic.RoleChangeFor(current, "1234", picked));

    [Fact]
    public void Page_NeverDrivesASelectorSelectionThroughXBind()
    {
        var xaml = ReadView("OhgShowPage.xaml");

        Assert.DoesNotContain("SelectedValue=\"{x:Bind", xaml, StringComparison.Ordinal);
        Assert.DoesNotContain("SelectedItem=\"{x:Bind", xaml, StringComparison.Ordinal);
        Assert.DoesNotContain("SelectedIndex=\"{x:Bind", xaml, StringComparison.Ordinal);

        // ...and the code-behind is where it IS applied.
        var code = ReadView("OhgShowPage.xaml.cs");
        Assert.Contains("SelectionChanged", code, StringComparison.Ordinal);
    }

    [Fact]
    public void Page_NamesEveryInteractiveElementForAutomation()
    {
        var xaml = ReadView("OhgShowPage.xaml");

        var unnamed = new List<string>();
        foreach (Match match in Regex.Matches(xaml, @"<(?:Button|ComboBox|MenuFlyoutItem|ToggleSwitch)(?=[\s/>])[^>]*>", RegexOptions.Singleline))
        {
            if (!match.Value.Contains("AutomationProperties.Name", StringComparison.Ordinal))
            {
                unnamed.Add(Collapse(match.Value));
            }
        }

        Assert.True(unnamed.Count == 0, "Unnamed interactive elements: " + string.Join(" | ", unnamed));
    }

    [Fact]
    public void Workspace_CarriesTheOhgShowNavButtonAndPageHost()
    {
        var xaml = ReadView("StudioWorkspace.xaml");

        Assert.Contains("CommandParameter=\"ohgshow\"", xaml, StringComparison.Ordinal);
        Assert.Contains("<views:OhgShowPage", xaml, StringComparison.Ordinal);
        Assert.Contains("ViewModel.IsOhgShowTab", xaml, StringComparison.Ordinal);
        Assert.Contains("ViewModel.OhgShowTabChrome", xaml, StringComparison.Ordinal);
    }

    // ── pure tab plumbing (no ViewModel instance — StudioViewModel is not constructible) ──

    [Theory]
    [InlineData("ohgshow", StudioTab.OhgShow)]
    [InlineData("automation", StudioTab.Automation)]
    [InlineData("settings", StudioTab.Settings)]
    [InlineData("scenes", StudioTab.Sources)]
    public void ParseTab_RoutesKnownKeys(string key, StudioTab expected)
        => Assert.Equal(expected, StudioTabPlumbing.ParseTab(key));

    [Theory]
    [InlineData("")]
    [InlineData(null)]
    [InlineData("nope")]
    [InlineData("OhgShow")]
    public void ParseTab_FallsBackToStudio(string? key)
        => Assert.Equal(StudioTab.Studio, StudioTabPlumbing.ParseTab(key));

    /// <summary>
    /// The tab-change notification set is a DATA list so it can be asserted without constructing
    /// the ViewModel (which field-initializes a <c>DispatcherQueue</c> and launches the core).
    /// Dropping OhgShow from the raises — the mutation this test exists for — means a nav click
    /// changes <c>ActiveTab</c> while the page host and the nav chrome never hear about it.
    /// </summary>
    [Fact]
    public void ActiveTabNotifications_IncludeTheOhgShowTabAndItsChrome()
    {
        Assert.Contains("IsOhgShowTab", StudioTabPlumbing.ActiveTabDependentProperties);
        Assert.Contains("OhgShowTabChrome", StudioTabPlumbing.ActiveTabDependentProperties);

        // Every tab in the enum must have both, so the next tab added cannot repeat this bug.
        foreach (var tab in Enum.GetValues<StudioTab>())
        {
            Assert.Contains($"Is{tab}Tab", StudioTabPlumbing.ActiveTabDependentProperties);
            Assert.Contains($"{tab}TabChrome", StudioTabPlumbing.ActiveTabDependentProperties);
        }
    }

    private static string Collapse(string text)
        => Regex.Replace(text, @"\s+", " ").Trim();

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
