using System.Diagnostics;
using System.Text.Json;
using CoreVideoPro.MediaCore.Services;
using CoreVideoPro.WinUI.Models;
using CoreVideoPro.WinUI.ViewModels;
using CoreVideoPro.WinUI.Views;
using CoreVideoPro.WinUI.Controls;
using CoreVideoPro.MediaCore.Models;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Media;
using Windows.Graphics;

namespace CoreVideoPro.WinUI.Services;

// Real compiled-XAML check. Owns an offscreen window, never starts a media core
// or attempts a real meeting. Only synthetic URL/ID values are used.
internal sealed class OperatorSettingsProbe(string reportPath)
{
    internal async Task RunAsync()
    {
        var checks = new List<string>(); string? error = null;
        var window = new Window();
        await using var bridge = new MediaCoreBridgeService();
        try
        {
            var editor = new SettingsViewModel(bridge, () => false);
            var page = new SettingsPage { ViewModel = editor };
            window.Content = page;
            window.AppWindow.MoveAndResize(new RectInt32(-20000, -20000, 1100, 760));
            window.AppWindow.Show(false);
            await Eventually(() => page.IsLoaded);
            var field = Descendants(page).OfType<TextBox>().Single(t => t.Name == "JoinMeetingField");
            field.Focus(FocusState.Programmatic);
            foreach (var value in new[] { "987654321", "https://zoom.us/j/987654321?pwd=synthetic", "invalid-synthetic-link" })
            {
                field.Text = value;
                Require(editor.JoinMeetingUrl == value, "Visible meeting value was not committed synchronously");
                Require(field.FocusState != FocusState.Unfocused, "Meeting field lost focus before command");
            }
            checks.Add("ID, URL and invalid edit update model before focus loss");
            await editor.JoinZoomCommand.ExecuteAsync(null);
            Require(editor.JoinStatus.Contains("valid Zoom", StringComparison.OrdinalIgnoreCase), "Join did not validate the current visible invalid value");
            checks.Add("first Join validates current visible field, without attempting a meeting");
            var health = Descendants(page).OfType<Expander>().Single(e => e.Name == "HealthSection");
            health.IsExpanded = true;
            await Eventually(() => Descendants(health).OfType<DiagnosticsView>().Any(v => v.IsLoaded));
            Require(Descendants(health).OfType<Button>().Any(b => b.Content?.ToString() == "Export support bundle"), "Support export is missing from Settings health");
            checks.Add("Settings health expands with the shared diagnostics and support export surface");
            window.AppWindow.Resize(new SizeInt32(900, 640));
            health.IsExpanded = false;
            checks.Add("Settings health collapse and narrow window resize");
            Require(Descendants(page).OfType<ToggleSwitch>().Any(t => t.Name == "OhgShowOption" && !t.IsOn), "Optional OHG workspace control is missing or not off by default");
            checks.Add("Settings exposes the default-off optional OHG workspace control");
            await CheckSourceOptions(window, checks);
            await CheckSourcesLayout(window, checks);
            await CheckLowerThirdText(window, checks);
            await CheckAdjustmentControls(window,checks);
        }
        catch (Exception ex) { error = ex.ToString(); }
        finally
        {
            Directory.CreateDirectory(Path.GetDirectoryName(Path.GetFullPath(reportPath))!);
            await File.WriteAllTextAsync(reportPath, JsonSerializer.Serialize(new { passed = error is null, checks, error }, new JsonSerializerOptions { WriteIndented = true }));
            if (error is not null) Environment.ExitCode = 1;
            window.Close();
        }
    }
    private static async Task CheckAdjustmentControls(Window window,List<string> checks)
    {
        var control=new SliderValueControl { Header="Precision",Minimum=0,Maximum=1,Value=.123456789,StepFrequency=.01 };
        var away=new Button { Content="Focus target" };var panel=new StackPanel();panel.Children.Add(control);panel.Children.Add(away);window.Content=panel;
        await Eventually(()=>control.IsLoaded);
        var field=Descendants(control).OfType<TextBox>().Single();
        field.Focus(FocusState.Programmatic);await Eventually(()=>field.FocusState!=FocusState.Unfocused);await Task.Delay(30);
        away.Focus(FocusState.Programmatic);await Eventually(()=>away.FocusState!=FocusState.Unfocused);await Task.Delay(30);Require(control.Value==.123456789,"Focusing exact entry quantized the saved value");
        field.Focus(FocusState.Programmatic);await Eventually(()=>field.FocusState!=FocusState.Unfocused);await Task.Delay(30);field.Text="NaN";await Task.Delay(30);
        away.Focus(FocusState.Programmatic);await Eventually(()=>away.FocusState!=FocusState.Unfocused);await Task.Delay(30);Require(control.Value==.123456789,"Invalid entry changed a parameter");
        field.Focus(FocusState.Programmatic);await Eventually(()=>field.FocusState!=FocusState.Unfocused);await Task.Delay(30);field.Text="0.314159";await Task.Delay(30);
        away.Focus(FocusState.Programmatic);await Eventually(()=>away.FocusState!=FocusState.Unfocused);await Eventually(()=>control.Value==.314159);Require(control.Value==.314159,$"Exact entry did not commit unchanged: {control.Value:G}");
        checks.Add("shared slider preserves untouched precision, rejects invalid entry and commits exact entry without step rounding");
        var designer=new LowerThirdDesignerControl();window.Content=new ScrollViewer { Content=designer };
        foreach(var id in new[] {"compact-solid","minimal-accent","broadcast"}) {
            designer.LoadReviewDraft(LowerThirdAppearance.FromPreset(id));await Eventually(()=>designer.IsLoaded);
            Require(Descendants(designer).OfType<SliderValueControl>().Count()==9,"Designer adjustment controls missing");
        }
        window.AppWindow.Resize(new SizeInt32(500,760));designer.UpdateLayout();
        Require(Descendants(designer).OfType<Button>().Any(b=>b.Content?.ToString()=="Apply look"),"Designer Apply missing");
        checks.Add("lower-third designer presets, sliders, color selectors and Apply load in real UI at narrow width without live state");
    }
    private static async Task CheckLowerThirdText(Window window, List<string> checks)
    {
        var slot = new ShowInputSlot { SlotNumber = 1, Kind = ShowInputKind.ZoomParticipant, ParticipantId = "synthetic-a" };
        var writes = new List<string>();
        var editor = new ShowInputSlotViewModel(slot, () => { }, applyText: (id, name, secondary) => writes.Add($"{id}:{name}:{secondary}"));
        editor.RefreshSourceOptions([new Participant { Id = "synthetic-a", Name = "Alice", Title = "Presenter" }], []);
        var owner = new Button { Content = "Lower-third text", Tag = editor };
        window.Content = owner; await Eventually(() => owner.IsLoaded);
        var flyout = SourceLowerThirdFlyout.Create(owner, editor);
        flyout.ShowAt(owner);
        var panel = (StackPanel)flyout.Content; await Eventually(() => panel.IsLoaded);
        var fields = panel.Children.OfType<TextBox>().ToArray();
        var useDefault = panel.Children.OfType<CheckBox>().Single();
        fields[0].Text = "Alice Smith"; useDefault.IsChecked = false; fields[1].Text = "";
        Require(writes.Count == 0, "Draft lower-third edits changed live state before Apply");
        var preview = ((StackPanel)panel.Children.OfType<Border>().Single().Child).Children.OfType<TextBlock>().ToArray();
        await Eventually(() => preview[0].Text == "Alice Smith" && preview[1].Visibility == Visibility.Collapsed);
        Require(preview[0].Text == "Alice Smith" && preview[1].Visibility == Visibility.Collapsed, "Lower-third blank preview retained metadata fallback");
        var apply = panel.Children.OfType<Button>().Single(b => b.Content?.ToString() == "Apply");
        var peer = new Microsoft.UI.Xaml.Automation.Peers.ButtonAutomationPeer(apply);
        ((Microsoft.UI.Xaml.Automation.Provider.IInvokeProvider)peer.GetPattern(Microsoft.UI.Xaml.Automation.Peers.PatternInterface.Invoke)).Invoke();
        await Eventually(() => writes.Count > 0);
        Require(writes.SequenceEqual(new[] { "zoom:synthetic-a:Alice Smith:" }), "Apply did not commit both lines once to the intended source");
        checks.Add("real lower-third editor previews hidden secondary text and commits both lines only on Apply");
        var stale = SourceLowerThirdFlyout.Create(owner, editor); stale.ShowAt(owner);
        var stalePanel = (StackPanel)stale.Content; await Eventually(() => stalePanel.IsLoaded);
        slot.ParticipantId = "synthetic-b";
        var staleApply = stalePanel.Children.OfType<Button>().Single(b => b.Content?.ToString() == "Apply");
        var stalePeer = new Microsoft.UI.Xaml.Automation.Peers.ButtonAutomationPeer(staleApply);
        ((Microsoft.UI.Xaml.Automation.Provider.IInvokeProvider)stalePeer.GetPattern(Microsoft.UI.Xaml.Automation.Peers.PatternInterface.Invoke)).Invoke();
        await Eventually(() => !staleApply.IsEnabled);
        Require(writes.Count == 1 && !staleApply.IsEnabled, "Stale lower-third editor changed a replacement guest");
        stale.Hide(); checks.Add("open lower-third editor rejects Apply after guest replacement");
    }
    private static async Task CheckSourcesLayout(Window window, List<string> checks)
    {
        var page = new SourcesInputsPage();
        window.Content = page;
        await Eventually(() => page.IsLoaded);
        var repeater = (ItemsRepeater)page.FindName("ShowInputEditorsRepeater");
        var slots = Enumerable.Range(1, 10).Select(number => new ShowInputSlot { SlotNumber = number }).ToArray();
        slots[0].Kind = ShowInputKind.ZoomParticipant; slots[0].ParticipantId = "synthetic-a";
        slots[1].Kind = ShowInputKind.UvcWebcam; slots[1].CaptureDeviceId = "synthetic-camera";
        slots[2].Kind = ShowInputKind.Media; slots[2].ParticipantId = "media:synthetic-clip";
        var editors = slots.Select(slot => new ShowInputSlotViewModel(slot, () => { })).ToArray();
        repeater.ItemsSource = editors;
        foreach (var width in new[] { 1100, 900 })
        {
            window.AppWindow.Resize(new SizeInt32(width, 760));
            page.UpdateLayout();
            for (var i = 0; i < 10; ++i)
            {
                var element = repeater.GetOrCreateElement(i);
                element.UpdateLayout();
                var button = Descendants(element).OfType<Button>().Single(b => b.Name == "SourceOptionsButton");
                Require(ReferenceEquals(button.Tag, editors[i]), "Source options template has a stale slot identity");
                Require(button.Visibility == (i == 0 ? Visibility.Visible : Visibility.Collapsed), "Source options appeared for a non-Zoom assignment");
                var textButton = Descendants(element).OfType<Button>().Single(b => b.Name == "SourceLowerThirdTextButton");
                Require(textButton.Visibility == (i < 2 ? Visibility.Visible : Visibility.Collapsed), "Lower-third editor advertised an unsupported or unassigned source");
            }
        }
        Require(slots[0].ParticipantId == "synthetic-a" && slots[1].CaptureDeviceId == "synthetic-camera" && slots[2].ParticipantId == "media:synthetic-clip", "Layout changed source assignments");
        checks.Add("compiled ten-row Sources template at 1100 and 900 pixels preserves synthetic Zoom/capture/media assignments and Zoom-only options");
    }
    private static async Task CheckSourceOptions(Window window, List<string> checks)
    {
        var slot = new ShowInputSlot { SlotNumber = 1, Kind = ShowInputKind.ZoomParticipant, ParticipantId = "synthetic-a" };
        var editor = new ShowInputSlotViewModel(slot, () => { });
        var owner = new Button { Content = "Options", Tag = editor };
        window.Content = owner;
        await Eventually(() => owner.IsLoaded);
        var writes = new List<string>();
        RouteSelectOption[] policies = [new() { Value = "hold", Label = "Hold last frame" }, new() { Value = "black", Label = "Black" }];
        var flyout = SourcePolicyFlyout.Create(owner, editor, ProductionRoleService.AssignmentOptions,
            policies, "host", "black", true,
            (id, value) => writes.Add($"role:{id}:{value}"), (id, value) => writes.Add($"policy:{id}:{value}"));
        flyout.ShowAt(owner);
        var panel = (StackPanel)flyout.Content;
        await Eventually(() => panel.IsLoaded);
        var combos = panel.Children.OfType<ComboBox>().ToArray();
        Require(combos[0].SelectedValue as string == "host" && combos[1].SelectedValue as string == "black", "Options lost current role or saved policy");
        Require(writes.Count == 0, "Opening options wrote back initial selections");
        combos[0].SelectedValue = "reader";
        combos[1].SelectedValue = "hold";
        Require(writes.SequenceEqual(new[] { "role:synthetic-a:reader", "policy:synthetic-a:hold" }), "Options did not target their original guest");
        checks.Add("real source options preserve selections and write only explicit edits to the selected guest");
        slot.ParticipantId = "synthetic-b";
        combos[1].SelectedValue = "black";
        Require(writes.Count == 2 && !combos[1].IsEnabled, "Reassignment redirected an open source policy edit");
        flyout.Hide();
        checks.Add("reassigned slot rejects edits from its old open flyout");

        // An ItemsRepeater can replace Tag without changing the old slot object.
        var recycled = SourcePolicyFlyout.Create(owner, editor, ProductionRoleService.AssignmentOptions,
            policies, "", "black", false, (_, _) => writes.Add("unexpected-role"), (_, _) => writes.Add("unexpected-policy"));
        recycled.ShowAt(owner);
        var recycledPanel = (StackPanel)recycled.Content;
        await Eventually(() => recycledPanel.IsLoaded);
        var recycledCombos = recycledPanel.Children.OfType<ComboBox>().ToArray();
        Require(!recycledCombos[0].IsEnabled && recycledCombos[1].SelectedValue as string == "black", "Unavailable guest lost saved policy or has an editable role");
        owner.Tag = new ShowInputSlotViewModel(new ShowInputSlot { SlotNumber = 2, Kind = ShowInputKind.ZoomParticipant, ParticipantId = "synthetic-b" }, () => { });
        recycledCombos[1].SelectedValue = "hold";
        Require(writes.Count == 2 && !recycledCombos[1].IsEnabled, "Recycled row redirected an old flyout edit");
        recycled.Hide();
        checks.Add("unavailable guest retains saved policy; recycled container rejects stale edits even for the same participant");
    }
    private static IEnumerable<DependencyObject> Descendants(DependencyObject root)
    {
        for (var i = 0; i < VisualTreeHelper.GetChildrenCount(root); ++i)
        {
            var child = VisualTreeHelper.GetChild(root, i); yield return child;
            foreach (var descendant in Descendants(child)) yield return descendant;
        }
    }
    private static async Task Eventually(Func<bool> ready)
    {
        var clock = Stopwatch.StartNew();
        while (!ready()) { if (clock.Elapsed > TimeSpan.FromSeconds(15)) throw new InvalidOperationException("Settings XAML did not load"); await Task.Delay(20); }
    }
    private static void Require(bool condition, string message) { if (!condition) throw new InvalidOperationException(message); }
}
