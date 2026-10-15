using System.Diagnostics;
using CoreVideoPro.WinUI.Controls;
using CoreVideoPro.WinUI.Models;
using CoreVideoPro.WinUI.ViewModels;
using CoreVideoPro.WinUI.Views;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Automation;
using Microsoft.UI.Xaml.Automation.Peers;
using Microsoft.UI.Xaml.Automation.Provider;
using Microsoft.UI.Xaml.Media;
using Windows.Graphics;

namespace CoreVideoPro.WinUI.Services;

internal static class SourcesWorkspaceIntegrationProbe
{
    private sealed class MemoryColumns : ISourcesWorkspacePreferenceStore
    {
        public SourcesWorkspacePreferences Value = new();
        public SourcesWorkspacePreferences Load() => Value;
        public void Save(SourcesWorkspacePreferences preferences) => Value = preferences;
    }
    internal static async Task CheckAsync(Window window, List<string> checks, string directory)
    {
        var store = new InMemoryProductionOutputPreferencesStore();
        await using var studio = new StudioViewModel(StudioStartupBootstrap.Create(store, startInteractiveServices: false));
        var slots = studio.ShowInputEditors.ToArray();
        var alice = new Participant { Id = "synthetic-a", Name = "Alice · test source", Health = FeedHealth.Live };
        var camera = new CaptureDevice { Id = "synthetic-camera", Name = "Studio camera", NativeDeviceId = "synthetic", Vendor = "uvc", Inputs = [], SelectedInputId = "" };
        var clip = new MediaAsset { Id = "synthetic-clip", Name = "Opening clip", Kind = "video" };
        typeof(StudioViewModel).GetProperty(nameof(StudioViewModel.RoomParticipantsForInputs))!.SetValue(studio, new Participant[] { alice });
        typeof(StudioViewModel).GetProperty(nameof(StudioViewModel.RoomVideoParticipants))!.SetValue(studio, new Participant[] { alice });
        typeof(StudioViewModel).GetProperty(nameof(StudioViewModel.MediaBinGroups))!.SetValue(studio,
            new MediaBinGroup[] { new() { Kind = "video", Label = "Video", Assets = [clip] } });
        studio.CaptureDevices.Add(camera);
        foreach (var input in slots) input.RefreshSourceOptions([alice], [camera], mediaAssets: [clip]);
        slots[0].SelectedUnifiedSourceId = "zoom:synthetic-a";
        slots[1].SelectedUnifiedSourceId = "capture:synthetic-camera";
        slots[2].SelectedUnifiedSourceId = "media:synthetic-clip";
        // The real coordinator refreshes the options from its roster after a write.
        // Rehydrate the synthetic roster explicitly; never start an interactive core.
        foreach (var input in slots) input.RefreshSourceOptions([alice], [camera], mediaAssets: [clip]);
        var columns = new MemoryColumns();
        var workspace = new SourcesWorkspaceControl(columns) { Studio = studio };
        window.Content = workspace; window.AppWindow.Resize(new SizeInt32(1600, 950));
        await Eventually(() => workspace.IsLoaded && Descendants(workspace).OfType<SourcesTableRowControl>().Count() == 10);
        var rows = Descendants(workspace).OfType<SourcesTableRowControl>().OrderBy(row => row.Input.SlotNumber).ToArray();
        Require(rows.Select(row => row.Input).SequenceEqual(slots), "Table changed slot object identity");
        var quickPicker = Descendants(rows[0]).OfType<SourceAssignmentPicker>().Single();
        Require(!workspace.State.InspectorOpen, "Overview unexpectedly requires Inspect");
        var untouched = slots.Skip(1).Select(s => (s.SourceId, s.InShow, s.IsoEnabled)).ToArray();
        var sceneBeforePatch = studio.ActiveSceneId;
        Require(quickPicker.IsEnabled && quickPicker.DisplayLabel == slots[0].OverviewName,
            "Row source selector is disabled or lost its display identity");
        quickPicker.PrepareMenu();
        var staleChoice = Choices(quickPicker).Single(item => item.Text.Contains("Opening clip"));
        var patchCamera = Choices(quickPicker).Single(item => item.Text.Contains("Studio camera"));
        var originalItems = quickPicker.SourceMenu.Items.ToArray();
        slots[0].RefreshSourceOptions([alice], [camera], mediaAssets: [clip]);
        Require(originalItems.SequenceEqual(quickPicker.SourceMenu.Items), "Roster refresh rebuilt an open quick picker");
        Require(slots[0].SourceId == "zoom:synthetic-a", "Picker hydration assigned a source");
        InvokeChoice(patchCamera); await Task.Delay(30);
        Require(slots[0].SourceId == "capture:synthetic-camera" && !workspace.State.InspectorOpen,
            "Inline picker did not patch its row without opening Inspect");
        InvokeChoice(staleChoice);
        Require(slots[0].SourceId == "capture:synthetic-camera", "Stale menu choice patched a replacement source");
        Require(untouched.SequenceEqual(slots.Skip(1).Select(s => (s.SourceId, s.InShow, s.IsoEnabled))) &&
            studio.ActiveSceneId == sceneBeforePatch, "Quick patch changed another input or Program");
        slots[0].SelectedUnifiedSourceId = "zoom:synthetic-a";
        foreach (var input in slots) input.RefreshSourceOptions([alice], [camera], mediaAssets: [clip]);
        checks.Add("inline categorized picker patches the retained row without Inspect, rejects stale choices and preserves other inputs/Program; roster refresh never rebuilds its open menu");
        var before = slots.Select(s => (s.SourceId, s.InShow, s.IsoEnabled)).ToArray();
        var program = studio.ActiveSceneId;
        Invoke(Descendants(rows[1]).OfType<Button>().Single(b => AutomationProperties.GetName(b) == "Inspect input 2"));
        await Eventually(() => ReferenceEquals(workspace.State.SelectedInput, slots[1]) && workspace.State.InspectorOpen);
        var grade = Descendants(workspace).OfType<Button>().Single(b => b.Name == "InspectorGradeSource");
        Require(grade.Tag as string == "capture:synthetic-camera", "Inspector Grade targets a different source");
        var rowGrade = Descendants(rows[0]).OfType<Button>().Single(b => AutomationProperties.GetName(b) == "Grade input 1");
        Require(rowGrade.Tag as string == "synthetic-a", "Overview Grade does not use existing canonical grade identity");
        workspace.State.Query = "Alice"; workspace.State.Category = "Zoom";
        Require(rows[0].Visibility == Visibility.Visible && rows[1].Visibility == Visibility.Collapsed, "Category/search filter did not change only row visibility");
        Require(ReferenceEquals(workspace.State.SelectedInput, slots[1]), "Filtering silently selected a different source");
        workspace.State.Query = ""; workspace.State.Category = "All sources";
        foreach (var width in new[] { 1600, 1100, 900, 720 })
        {
            window.AppWindow.Resize(new SizeInt32(width, 950));
            foreach (var open in new[] { false, true })
            {
                workspace.State.InspectorOpen = open; workspace.UpdateLayout(); await Task.Delay(80);
                Require(ReferenceEquals(rows[1], Descendants(workspace).OfType<SourcesTableRowControl>().Single(r => r.Input.SlotNumber == 2)), "Resize recreated source controls");
                foreach (var row in rows)
                    Require(row.ActualWidth <= workspace.ActualWidth + 1, $"Source row overflow at {width}");
                Require(grade.Tag as string == "capture:synthetic-camera", "Resize changed grade target");
            }
        }
        Require(before.SequenceEqual(slots.Select(s => (s.SourceId, s.InShow, s.IsoEnabled))) && program == studio.ActiveSceneId, "Inspection/filter/resize changed production state");
        checks.Add("native table and inspector retain ten row objects, source-grade identities, assignments, MV/ISO and Program across selection/filtering and 1600/1100/900/720 widths");
        workspace.State.Inspect(slots[0]);
        slots[0].RefreshSourceOptions([alice], [camera], mediaAssets: [clip]);
        var sourceName = slots[0].SelectedUnifiedSourceLabel;
        slots[0].InShow = true;
        typeof(StudioViewModel).GetProperty(nameof(StudioViewModel.RoomParticipantsForInputs))!.SetValue(studio, Array.Empty<Participant>());
        slots[0].RefreshSourceOptions([], []); await Task.Delay(40);
        Require(slots[0].OverviewName.Contains("Alice"), "Offline input lost its remembered identity");
        var picker = Descendants(workspace).OfType<SourceAssignmentPicker>().Single(b => b.Name == "SelectedSourcePicker");
        Require(picker.DisplayLabel == sourceName + " · Offline", $"Inspector picker does not identify the offline binding: actual={picker.DisplayLabel}; expected={sourceName} · Offline; model={slots[0].SelectedUnifiedSourceLabel}");
        typeof(StudioViewModel).GetProperty(nameof(StudioViewModel.RoomParticipantsForInputs))!.SetValue(studio, new Participant[] { alice });
        slots[0].RefreshSourceOptions([alice], []); await Task.Delay(40);
        Require(picker.DisplayLabel == sourceName && ReferenceEquals(workspace.State.SelectedInput, slots[0]), "Reconnect changed selection or retained an offline label");
        checks.Add("compiled Sources table/inspector preserve a named offline Zoom binding and reconnect without substitution");
        var rename = Descendants(workspace).OfType<TextBox>().Single(t => t.Name == "SelectedSourceName");
        rename.Focus(FocusState.Programmatic); await Task.Delay(30); rename.Text = "Stale edit must not follow replacement";
        slots[0].SelectedUnifiedSourceId = "capture:synthetic-camera"; await Task.Delay(30);
        grade.Focus(FocusState.Programmatic); await Task.Delay(30);
        Require(slots[0].DisplayName != "Stale edit must not follow replacement", "Pending rename changed a replacement source");
        slots[0].SelectedUnifiedSourceId = "zoom:synthetic-a";
        foreach (var input in slots) input.RefreshSourceOptions([alice], [camera], mediaAssets: [clip]);
        checks.Add("compiled pending name edit rejects source replacement rather than renaming the new source");

        var mv = Descendants(rows[0]).OfType<CheckBox>().Single(c => AutomationProperties.GetName(c) == "Input 1 in Multiview");
        mv.IsChecked = false; Require(!slots[0].InShow && slots[0].SourceId == "zoom:synthetic-a", "Quick MV did not preserve binding");
        var iso = Descendants(rows[0]).OfType<CheckBox>().Single(c => AutomationProperties.GetName(c) == "Arm ISO for input 1");
        iso.IsChecked = true; Require(slots[0].IsoEnabled, "Quick ISO did not update the source selection");
        Require(slots[0].Inspector.RecChipLabel != "REC", "ISO selection invented recording state");
        checks.Add("compiled quick MV and ISO controls target the original source and distinguish arm selection from measured recording state");
        window.AppWindow.Resize(new SizeInt32(1600, 950)); workspace.State.InspectorOpen = false;
        await ReviewImageWriter.SaveAsync(workspace, Path.Combine(directory, "sources-table-native.png"));
        workspace.State.InspectorOpen = true;
        await ReviewImageWriter.SaveAsync(workspace, Path.Combine(directory, "sources-inspector-native.png"));
        workspace.State.Columns = new() { ShowType = false, ShowAudio = false };
        Invoke(Descendants(rows[1]).OfType<Button>().Single(b => AutomationProperties.GetName(b) == "More details for input 2"));
        Require(Descendants(rows[1]).OfType<TextBlock>().Any(t => t.Text == "Paired audio"), "Hidden column details are missing");
        checks.Add("secondary columns and expandable native row details retain audio/type/state information");
        window.Content = new SourcesInputsPage { ViewModel = studio }; await Task.Delay(100);
        var catalogs = Descendants(window.Content).OfType<Expander>().Select(e => e.Name).ToArray();
        Require(new[] { "CaptureCatalog", "SrtCatalog", "RtmpCatalog" }.All(catalogs.Contains), "Adapter setup catalog was removed");
        checks.Add("existing capture/browser, SRT and RTMP setup surfaces remain reachable in the Sources page");
    }
    private static IEnumerable<MenuFlyoutItem> Choices(SourceAssignmentPicker picker) =>
        picker.SourceMenu.Items.OfType<MenuFlyoutSubItem>().SelectMany(group => group.Items.OfType<MenuFlyoutItem>());
    private static void InvokeChoice(MenuFlyoutItem item) =>
        ((IInvokeProvider)new MenuFlyoutItemAutomationPeer(item).GetPattern(PatternInterface.Invoke)).Invoke();
    private static void Invoke(Button button) => ((IInvokeProvider)new ButtonAutomationPeer(button).GetPattern(PatternInterface.Invoke)).Invoke();
    private static async Task Eventually(Func<bool> condition)
    { var timer = Stopwatch.StartNew(); while (!condition()) { if (timer.ElapsedMilliseconds > 5000) throw new InvalidOperationException("Sources integration condition timed out"); await Task.Delay(20); } }
    private static void Require(bool condition, string message) { if (!condition) throw new InvalidOperationException(message); }
    private static IEnumerable<DependencyObject> Descendants(DependencyObject root)
    { for (var i = 0; i < VisualTreeHelper.GetChildrenCount(root); i++) { var child = VisualTreeHelper.GetChild(root, i); yield return child; foreach (var item in Descendants(child)) yield return item; } }
}
