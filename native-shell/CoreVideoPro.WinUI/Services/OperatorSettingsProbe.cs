using System.Diagnostics;
using System.Text.Json;
using CoreVideoPro.MediaCore.Services;
using CoreVideoPro.WinUI.Models;
using CoreVideoPro.WinUI.ViewModels;
using CoreVideoPro.WinUI.Views;
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
            await CheckSourceOptions(window, checks);
            await CheckSourcesLayout(window, checks);
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
