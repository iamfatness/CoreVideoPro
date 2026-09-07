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

        var handlers = Regex.Matches(xaml, @"(?:Click|Loaded|SelectionChanged|ElementPrepared|Tapped|Toggled)=""(\w+)""")
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

        // Task 8's handlers - the look picker, the two toggles, and the override role picker.
        Assert.Contains("Guarded(\"look change\"", code, StringComparison.Ordinal);
        Assert.Contains("Guarded(\"look combo sync\"", code, StringComparison.Ordinal);
        Assert.Contains("Guarded(\"as-follow toggle\"", code, StringComparison.Ordinal);
        Assert.Contains("Guarded(\"smart gallery toggle\"", code, StringComparison.Ordinal);
        Assert.Contains("Guarded(\"override role change\"", code, StringComparison.Ordinal);
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

    // -- Task 8: program / gallery / GFX panels ----------------------------------------

    [Fact]
    public void Page_BindsTheProgramPanel()
    {
        var xaml = ReadView("OhgShowPage.xaml");

        // Readouts: what is on air, what is cued, who is talking, who is queued.
        Assert.Contains("ViewModel.OhgShow.ProgramLabel", xaml, StringComparison.Ordinal);
        Assert.Contains("ViewModel.OhgShow.PreviewLabel", xaml, StringComparison.Ordinal);
        Assert.Contains("ViewModel.OhgShow.CurrentSpeakerLabel", xaml, StringComparison.Ordinal);
        Assert.Contains("ViewModel.OhgShow.QueueLabel", xaml, StringComparison.Ordinal);

        // Take surface.
        Assert.Contains("ViewModel.OhgShow.CutCommand", xaml, StringComparison.Ordinal);
        Assert.Contains("ViewModel.OhgShow.AutoCommand", xaml, StringComparison.Ordinal);
        Assert.Contains("ViewModel.OhgShow.PreviewCommand", xaml, StringComparison.Ordinal);
        Assert.Contains("ViewModel.OhgShow.DirectCutCommand", xaml, StringComparison.Ordinal);

        // Direct cut takes the PVW SOURCE - the raw wire string, never the formatted label.
        Assert.Contains("ViewModel.OhgShow.PreviewSource", xaml, StringComparison.Ordinal);

        // Look + paging + boxes.
        Assert.Contains("ViewModel.OhgShow.Boxes", xaml, StringComparison.Ordinal);
        Assert.Contains("ViewModel.OhgShow.NextGuestCommand", xaml, StringComparison.Ordinal);
        Assert.Contains("ViewModel.OhgShow.PrevGuestCommand", xaml, StringComparison.Ordinal);
        Assert.Contains("ViewModel.OhgShow.AssignSelectedSlotToBoxCommand", xaml, StringComparison.Ordinal);
        Assert.Contains("ViewModel.OhgShow.ClearBoxCommand", xaml, StringComparison.Ordinal);

        // The AS-follow switch reads its state OneWay and applies in code-behind.
        Assert.Contains("ViewModel.OhgShow.AsFollow", xaml, StringComparison.Ordinal);

        var code = ReadView("OhgShowPage.xaml.cs");
        Assert.Contains("SetAsFollowCommand", code, StringComparison.Ordinal);

        // The look picker is code-behind driven (ItemsSource + selection), never x:Bind.
        Assert.Contains("SetLookCommand", code, StringComparison.Ordinal);
        Assert.Contains("show.Looks", code, StringComparison.Ordinal);
    }

    [Fact]
    public void Page_BindsTheGalleryPanel()
    {
        var xaml = ReadView("OhgShowPage.xaml");

        Assert.Contains("ViewModel.OhgShow.Gallery,", xaml, StringComparison.Ordinal);
        Assert.Contains("ViewModel.OhgShow.GalleryNote", xaml, StringComparison.Ordinal);
        Assert.Contains("ViewModel.OhgShow.ReplaceCellWithSelectedSlotCommand", xaml, StringComparison.Ordinal);
        Assert.Contains("ViewModel.OhgShow.RemoveCellCommand", xaml, StringComparison.Ordinal);
        Assert.Contains("ViewModel.OhgShow.ResetGalleryFromSlotsCommand", xaml, StringComparison.Ordinal);
        Assert.Contains("ViewModel.OhgShow.EmptyGalleryCommand", xaml, StringComparison.Ordinal);

        // Smart gallery: OneWay state in, Toggled out through code-behind.
        Assert.Contains("ViewModel.OhgShow.SmartGallery", xaml, StringComparison.Ordinal);

        var code = ReadView("OhgShowPage.xaml.cs");
        Assert.Contains("SetSmartGalleryCommand", code, StringComparison.Ordinal);
    }

    [Fact]
    public void Page_BindsTheGfxAndDataPanel()
    {
        var xaml = ReadView("OhgShowPage.xaml");

        // Question card.
        Assert.Contains("ViewModel.OhgShow.QuestionText", xaml, StringComparison.Ordinal);
        Assert.Contains("ViewModel.OhgShow.QuestionAsker", xaml, StringComparison.Ordinal);
        Assert.Contains("ViewModel.OhgShow.QuestionInCommand", xaml, StringComparison.Ordinal);
        Assert.Contains("ViewModel.OhgShow.QuestionOutCommand", xaml, StringComparison.Ordinal);

        // Headline editor - the two fields are TwoWay so the operator's typing reaches the VM.
        Assert.Contains("ViewModel.OhgShow.HeadlineName, Mode=TwoWay", xaml, StringComparison.Ordinal);
        Assert.Contains("ViewModel.OhgShow.HeadlineLocation, Mode=TwoWay", xaml, StringComparison.Ordinal);
        Assert.Contains("ViewModel.OhgShow.HeadlineVisible", xaml, StringComparison.Ordinal);
        Assert.Contains("ViewModel.OhgShow.HeadlineInCommand", xaml, StringComparison.Ordinal);
        Assert.Contains("ViewModel.OhgShow.HeadlineOutCommand", xaml, StringComparison.Ordinal);
        Assert.Contains("ViewModel.OhgShow.HeadlineChangeCommand", xaml, StringComparison.Ordinal);

        // Mukana health + the three capability lamps (with their detail text).
        Assert.Contains("ViewModel.OhgShow.MukanaHealthLabel", xaml, StringComparison.Ordinal);
        Assert.Contains("ViewModel.OhgShow.RegistryLamp", xaml, StringComparison.Ordinal);
        Assert.Contains("ViewModel.OhgShow.HandsLamp", xaml, StringComparison.Ordinal);
        Assert.Contains("ViewModel.OhgShow.QuestionLamp", xaml, StringComparison.Ordinal);
        Assert.Contains("ViewModel.OhgShow.MukanaSyncCommand", xaml, StringComparison.Ordinal);
        Assert.Contains("LampBrush(", xaml, StringComparison.Ordinal);

        // Registry override editor.
        Assert.Contains("ViewModel.OhgShow.OverridePin, Mode=TwoWay", xaml, StringComparison.Ordinal);
        Assert.Contains("ViewModel.OhgShow.OverrideName, Mode=TwoWay", xaml, StringComparison.Ordinal);
        Assert.Contains("ViewModel.OhgShow.OverrideLocation, Mode=TwoWay", xaml, StringComparison.Ordinal);
        Assert.Contains("ViewModel.OhgShow.OverrideSetCommand", xaml, StringComparison.Ordinal);
        Assert.Contains("ViewModel.OhgShow.OverrideDeleteCommand", xaml, StringComparison.Ordinal);

        // The override ROLE is a ComboBox, so it is applied in code-behind like every other
        // Selector on this page.
        var code = ReadView("OhgShowPage.xaml.cs");
        Assert.Contains("OverrideRole", code, StringComparison.Ordinal);
    }

    /// <summary>
    /// The five panel titles. The board keeps the two titles Task 7 named it with
    /// ("Panelists" over the roster, "Seats" over the seat grid); Task 8 adds the other three.
    /// </summary>
    [Fact]
    public void Page_TitlesEveryPanel()
    {
        var xaml = ReadView("OhgShowPage.xaml");

        foreach (var title in new[] { "Panelists", "Seats", "Program", "Gallery", "GFX &amp; data" })
        {
            Assert.Contains($"Text=\"{title}\"", xaml, StringComparison.Ordinal);
        }
    }

    /// <summary>
    /// The guard rule has a BLIND SPOT the XAML-text test above cannot see: a callback wired
    /// through <c>DependencyProperty.Register</c> never appears in the XAML, yet WinUI invokes it
    /// exactly like a Click handler - while it is setting the property, on the UI thread, with a
    /// throw fail-fasting the process and no managed stack. This closes it: every method the
    /// code-behind hands to a <c>PropertyMetadata</c>/<c>PropertyChangedCallback</c> must route
    /// through <c>Guarded</c>.
    /// </summary>
    [Fact]
    public void Page_GuardsEveryDependencyPropertyCallback()
    {
        var code = ReadView("OhgShowPage.xaml.cs");

        var callbacks = Regex.Matches(code, @"new\s+(?:PropertyMetadata|PropertyChangedCallback)\s*\(([^)]*)\)")
            .SelectMany(match => Regex.Matches(match.Groups[1].Value, @"[A-Za-z_]\w*").Select(m => m.Value))
            .Where(name => code.Contains($"void {name}(", StringComparison.Ordinal))
            .Distinct()
            .ToList();

        // The page HAS such a callback; an empty list would make this test vacuously green.
        Assert.NotEmpty(callbacks);

        foreach (var name in callbacks)
        {
            var body = MethodBody(code, name);
            Assert.True(
                body.Contains("Guarded(", StringComparison.Ordinal),
                $"DependencyProperty callback {name} is not routed through Guarded(...): {Collapse(body)}");
        }
    }

    /// <summary>The text of a method's body, by brace matching from its declaration - enough to
    /// assert what a callback does without compiling the page (which this project cannot do).</summary>
    private static string MethodBody(string code, string methodName)
    {
        var declaration = code.IndexOf($"void {methodName}(", StringComparison.Ordinal);
        Assert.True(declaration >= 0, $"Could not find a declaration for {methodName}.");

        var open = code.IndexOf('{', declaration);
        Assert.True(open >= 0, $"Could not find a body for {methodName}.");

        var depth = 0;
        for (var index = open; index < code.Length; index++)
        {
            if (code[index] == '{') depth++;
            else if (code[index] == '}' && --depth == 0) return code[open..(index + 1)];
        }

        throw new Xunit.Sdk.XunitException($"Unbalanced braces reading the body of {methodName}.");
    }

    // -- the Task 8 decisions, extracted and tested --

    [Fact]
    public void LookChangeFor_SendsThePickedLook()
        => Assert.Equal("panel4", OhgShowPageLogic.LookChangeFor("panel2", "panel4"));

    [Theory]
    [InlineData("panel2", "panel2")]   // re-selecting the cued look is not an edit
    [InlineData("panel2", null)]       // a ComboBox mid-rebind reports no selection
    [InlineData("panel2", "")]
    [InlineData("panel2", "   ")]
    [InlineData(null, null)]
    public void LookChangeFor_SendsNothingWhenThereIsNoRealChange(string? current, string? picked)
        => Assert.Null(OhgShowPageLogic.LookChangeFor(current, picked));

    [Fact]
    public void LookChangeFor_SendsTheFirstLookWhenNothingIsCued()
        => Assert.Equal("panel2", OhgShowPageLogic.LookChangeFor(null, "panel2"));

    [Theory]
    [InlineData(true, true)]
    [InlineData(false, false)]
    public void ToggleChangeFor_SendsNothingWhenTheSwitchAlreadyMatchesTheEngine(bool current, bool isOn)
        => Assert.Null(OhgShowPageLogic.ToggleChangeFor(current, isOn));

    [Theory]
    [InlineData(false, true)]
    [InlineData(true, false)]
    public void ToggleChangeFor_SendsTheOperatorsNewValue(bool current, bool isOn)
        => Assert.Equal(isOn, OhgShowPageLogic.ToggleChangeFor(current, isOn));

    [Theory]
    [InlineData("available", "StudioLiveBrush")]
    [InlineData("AVAILABLE", "StudioLiveBrush")]
    [InlineData("unavailable", "StudioMutedBrush")]
    [InlineData("disabled", "StudioProgramBrush")]
    [InlineData("", "StudioMutedBrush")]
    [InlineData(null, "StudioMutedBrush")]
    [InlineData("something-new", "StudioMutedBrush")]
    public void LampBrushKey_ColoursEveryCapabilityState(string? state, string expected)
        => Assert.Equal(expected, OhgShowPageLogic.LampBrushKey(state));

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
