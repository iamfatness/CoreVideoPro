using System.Text.RegularExpressions;
using Xunit;

namespace CoreVideoPro.WinUI.Tests;

/// <summary>
/// Plan 7b Task 10 — the OHG section of the production settings window. XAML-TEXT assertions, the
/// <see cref="OhgShowPageContentTests"/> pattern: this project cannot compile a WinUI page, so what
/// it holds is the page's CONTRACT with <c>OhgSettingsViewModel</c> plus the two house rules that
/// have historically cost a show — no <c>Selector</c> selection driven by x:Bind, and an
/// <c>AutomationProperties.Name</c> on every interactive element.
/// </summary>
public sealed class OhgSettingsSectionContentTests
{
    [Fact]
    public void Window_HasAnOhgMenuButtonAndPanel()
    {
        var xaml = ReadView("ProductionSettingsWindow.xaml");
        var code = ReadView("ProductionSettingsWindow.xaml.cs");

        Assert.Contains("x:Name=\"OhgMenuButton\"", xaml, StringComparison.Ordinal);
        Assert.Contains("Click=\"OnOhgClicked\"", xaml, StringComparison.Ordinal);
        Assert.Contains("x:Name=\"OhgPanel\"", xaml, StringComparison.Ordinal);

        // ShowSection("ohg") must actually route to it, and ShowPanel must hide it with the rest.
        Assert.Contains("\"ohg\" => OhgPanel", code, StringComparison.Ordinal);
        Assert.Contains("OhgPanel.Visibility", code, StringComparison.Ordinal);
    }

    [Fact]
    public void Window_ExposesTheSettingsViewModel()
    {
        var code = ReadView("ProductionSettingsWindow.xaml.cs");

        Assert.Contains("public OhgSettingsViewModel? OhgSettings { get; }", code, StringComparison.Ordinal);
        Assert.Contains("CreateOhgSettingsViewModel()", code, StringComparison.Ordinal);
    }

    [Fact]
    public void Section_BindsTheSaveAndImportSurface()
    {
        var xaml = ReadView("ProductionSettingsWindow.xaml");

        Assert.Contains("OhgSettings.SaveCommand", xaml, StringComparison.Ordinal);
        Assert.Contains("OhgSettings.ValidationMessage", xaml, StringComparison.Ordinal);
        Assert.Contains("OhgSettings.SaveStatus", xaml, StringComparison.Ordinal);
        Assert.Contains("OhgSettings.NeedsAppRestart", xaml, StringComparison.Ordinal);
        Assert.Contains("OhgSettings.LoadedFromDefaultsBecauseOfError", xaml, StringComparison.Ordinal);
        Assert.Contains("OhgSettings.AddLookCommand", xaml, StringComparison.Ordinal);

        // Import legacy show is a Click handler, not a Command binding - a Command binding can't
        // collect two optional FileOpenPicker results before invoking the VM. Assert the EXACT
        // handler name is wired (not a comment mentioning the command) and that the handler's own
        // code actually reaches ImportLegacyCommand.
        Assert.Contains("Click=\"OnOhgImportLegacyClicked\"", xaml, StringComparison.Ordinal);
        Assert.DoesNotContain("OhgSettings.ImportLegacyCommand", xaml, StringComparison.Ordinal);

        // Two commands are invoked from guarded Click handlers rather than bound: remove-look,
        // because inside an ItemsRepeater template there is no element to name for an ElementName
        // binding back out to the window; and refresh-scenes, because Button.Command runs AFTER
        // the Click handler, so the handler that redraws the pickers has to invoke it itself.
        var code = ReadView("ProductionSettingsWindow.xaml.cs");
        Assert.Contains("RemoveLookCommand", code, StringComparison.Ordinal);
        Assert.Contains("RefreshScenesCommand", code, StringComparison.Ordinal);

        // The Click handler's own code (not a comment) must reach ImportLegacyCommand.
        Assert.Contains("void OnOhgImportLegacyClicked(", code, StringComparison.Ordinal);
        Assert.Contains("ImportLegacyCommand.ExecuteAsync(", code, StringComparison.Ordinal);
    }

    [Fact]
    public void Section_BindsTheEditableFieldsThroughTheViewModelNotTheModel()
    {
        var xaml = ReadView("ProductionSettingsWindow.xaml");

        // OhgConfigEditModel is a plain mutable class with NO INotifyPropertyChanged, so binding
        // OhgSettings.Model.X TwoWay would write once and never notify. Every editable field is
        // mirrored on the view model (which writes through to Model).
        Assert.DoesNotContain("OhgSettings.Model.", xaml, StringComparison.Ordinal);

        foreach (var field in new[]
                 {
                     "OhgSettings.RegistryEnabled", "OhgSettings.HandsQueueEnabled", "OhgSettings.QuestionFeedEnabled",
                     "OhgSettings.MukanaBaseUrl", "OhgSettings.MukanaEvent",
                     "OhgSettings.PanelistsIntervalMs", "OhgSettings.HandsIntervalMs",
                     "OhgSettings.QuestionIntervalMs", "OhgSettings.MaxBackoffMs",
                     "OhgSettings.DriveHost", "OhgSettings.TallyUrl",
                 })
        {
            Assert.Contains(field, xaml, StringComparison.Ordinal);
        }

        // Shadow mode is the DEFAULT and the safe state; the switch has to say so.
        Assert.Contains("Drive the show (off = shadow mode)", xaml, StringComparison.Ordinal);
    }

    [Fact]
    public void Section_RepeatsTheLooksEditor()
    {
        var xaml = ReadView("ProductionSettingsWindow.xaml");

        Assert.Contains("OhgSettings.Looks", xaml, StringComparison.Ordinal);

        foreach (var field in new[] { "Id", "Label", "BoxesValue", "IncludesHost", "IncludesReader" })
        {
            Assert.Contains($"x:Bind {field},", xaml, StringComparison.Ordinal);
        }
    }

    /// <summary>
    /// CLAUDE.md, live-QA day: a recycled ItemsRepeater container re-bound while its ItemsSource
    /// was still resolving threw COMException 0x80004005 out of <c>Selector.set_SelectedValue</c>
    /// and killed the app. Every ComboBox on this section is applied in code-behind, guarded.
    /// </summary>
    [Fact]
    public void Section_NeverDrivesASelectorSelectionThroughXBind()
    {
        // Scoped to the OHG section: the older panels in this window predate the rule and are not
        // in this task's scope (they are plain top-level ComboBoxes, not repeater templates).
        var section = OhgSection(ReadView("ProductionSettingsWindow.xaml"));

        Assert.DoesNotContain("SelectedValue=\"{x:Bind", section, StringComparison.Ordinal);
        Assert.DoesNotContain("SelectedItem=\"{x:Bind", section, StringComparison.Ordinal);
        Assert.DoesNotContain("SelectedIndex=\"{x:Bind", section, StringComparison.Ordinal);

        var code = ReadView("ProductionSettingsWindow.xaml.cs");
        Assert.Contains("SelectionChanged", code, StringComparison.Ordinal);
    }

    [Fact]
    public void Section_NamesEveryInteractiveElementForAutomation()
    {
        var section = OhgSection(ReadView("ProductionSettingsWindow.xaml"));

        var unnamed = new List<string>();
        foreach (Match match in Regex.Matches(section, @"<(?:Button|ComboBox|MenuFlyoutItem|ToggleSwitch|NumberBox|TextBox)(?=[\s/>])[^>]*>", RegexOptions.Singleline))
        {
            if (!match.Value.Contains("AutomationProperties.Name", StringComparison.Ordinal))
            {
                unnamed.Add(Collapse(match.Value));
            }
        }

        Assert.True(unnamed.Count == 0, "Unnamed interactive elements in the OHG section: " + string.Join(" | ", unnamed));
    }

    /// <summary>Every interactive element in the section is named "OHG settings ..." so the
    /// operator's screen reader (and our own UIA tooling) can tell them apart from the identically
    /// labelled controls in the other six sections of the same window.</summary>
    [Fact]
    public void Section_PrefixesEveryAutomationNameSoItIsUnambiguous()
    {
        var section = OhgSection(ReadView("ProductionSettingsWindow.xaml"));

        var names = Regex.Matches(section, @"AutomationProperties\.Name=""([^""]+)""")
            .Select(match => match.Groups[1].Value)
            .ToList();

        Assert.NotEmpty(names);
        foreach (var name in names)
        {
            Assert.StartsWith("OHG settings ", name, StringComparison.Ordinal);
        }
    }

    /// <summary>
    /// A throwing UI callback fail-fasts the process with NO managed log (CLAUDE.md, UiDispatch).
    /// Every handler this window hooks from the OHG section must route through <c>Guarded</c>.
    /// </summary>
    [Fact]
    public void Window_GuardsEveryOhgUiCallback()
    {
        var xaml = ReadView("ProductionSettingsWindow.xaml");
        var code = ReadView("ProductionSettingsWindow.xaml.cs");

        Assert.Contains("private void Guarded(", code, StringComparison.Ordinal);
        Assert.Contains("LaunchLog.Write", code, StringComparison.Ordinal);

        var handlers = Regex.Matches(OhgSection(xaml), @"(?:Click|Loaded|SelectionChanged|Toggled|TextChanged|ValueChanged)=""(\w+)""")
            .Select(match => match.Groups[1].Value)
            .Distinct()
            .ToList();

        Assert.NotEmpty(handlers);
        foreach (var handler in handlers)
        {
            var body = MethodBody(code, handler);
            Assert.True(
                body.Contains("Guarded(", StringComparison.Ordinal),
                $"XAML handler {handler} is not routed through Guarded(...): {Collapse(body)}");
        }
    }

    /// <summary>The OHG panel's XAML, from its ScrollViewer to the end of that ScrollViewer — the
    /// other six sections predate these rules and are not in scope for this test.</summary>
    private static string OhgSection(string xaml)
    {
        var start = xaml.IndexOf("x:Name=\"OhgPanel\"", StringComparison.Ordinal);
        Assert.True(start >= 0, "The settings window has no OhgPanel.");
        var end = xaml.IndexOf("</ScrollViewer>", start, StringComparison.Ordinal);
        Assert.True(end >= 0, "The OhgPanel ScrollViewer is not closed.");
        return xaml[start..end];
    }

    private static string MethodBody(string code, string methodName)
    {
        var declaration = code.IndexOf($"void {methodName}(", StringComparison.Ordinal);
        Assert.True(declaration >= 0, $"Could not find a declaration for {methodName}.");

        // Handlers here are expression-bodied (`=> Guarded("...", () => { ... })`), so brace
        // matching alone would return the LAMBDA body and miss the Guarded call that wraps it.
        var arrow = code.IndexOf("=>", declaration, StringComparison.Ordinal);
        var open = code.IndexOf('{', declaration);
        if (arrow >= 0 && (open < 0 || arrow < open))
        {
            var depth = 0;
            for (var index = arrow; index < code.Length; index++)
            {
                var c = code[index];
                if (c is '{' or '(') depth++;
                else if (c is '}' or ')') depth--;
                else if (c == ';' && depth == 0) return code[arrow..(index + 1)];
            }

            throw new Xunit.Sdk.XunitException($"Unterminated expression body for {methodName}.");
        }

        Assert.True(open >= 0, $"Could not find a body for {methodName}.");

        var braces = 0;
        for (var index = open; index < code.Length; index++)
        {
            if (code[index] == '{') braces++;
            else if (code[index] == '}' && --braces == 0) return code[open..(index + 1)];
        }

        throw new Xunit.Sdk.XunitException($"Unbalanced braces reading the body of {methodName}.");
    }

    private static string Collapse(string text) => Regex.Replace(text, @"\s+", " ").Trim();

    private static string ReadView(string fileName)
    {
        for (var directory = new DirectoryInfo(AppContext.BaseDirectory);
             directory is not null;
             directory = directory.Parent)
        {
            var candidate = Path.Combine(directory.FullName, "native-shell", "CoreVideoPro.WinUI", "Views", fileName);
            if (File.Exists(candidate))
            {
                return File.ReadAllText(candidate);
            }
        }

        throw new FileNotFoundException($"Could not locate {fileName} from the test output directory.");
    }
}
