using System.Diagnostics;
using CoreVideoPro.WinUI.Models;
using CoreVideoPro.WinUI.ViewModels;
using CoreVideoPro.WinUI.ViewModels.Transport;
using CoreVideoPro.WinUI.Views;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Automation.Peers;
using Microsoft.UI.Xaml.Automation.Provider;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Media;

namespace CoreVideoPro.WinUI.Services;

// Actual compiled controls and production persistence; an isolated in-memory
// show, no native process, device, meeting or owner preference file.
internal static class SceneNamingIntegrationProbe
{
    private sealed class MemoryStore : IProductionOutputPreferencesStore
    {
        public ProductionOutputPreferences Value { get; private set; } = new();
        public ProductionOutputPreferences? Load() => Value;
        public void Save(ProductionOutputPreferences value) => Value =
            ProductionOutputPreferencesSerializer.Deserialize(ProductionOutputPreferencesSerializer.Serialize(value))!;
    }

    internal static async Task CheckAsync(Window window, List<string> checks, string? reviewDirectory = null)
    {
        var store = new MemoryStore();
        await using var studio = new StudioViewModel(StudioStartupBootstrap.Create(store, startInteractiveServices: false));
        var page = new SourcesPage { ViewModel = studio };
        window.Content = page;
        window.AppWindow.Resize(new Windows.Graphics.SizeInt32(1366, 768));
        await Eventually(() => page.IsLoaded && page.Workspace is not null);
        Require(page.FindName("RenameSceneButton") is Button, "Scene Builder has no Rename scene action (#1196)");
        Require(((MenuFlyout)((Button)page.FindName("NewSceneButton")).Flyout).Items.OfType<MenuFlyoutItem>()
            .Any(item => item.Tag as string == "tiles"), "New scene chooser has no CoreVideo Tiles starter (#1199)");
        var programId = studio.ActiveSceneId;
        var previewId = studio.PreviewSceneId;
        var count = studio.Scenes.Count;

        async Task<ContentDialog> OpenCreate(string template = "solo")
        {
            var button = (Button)page.FindName("NewSceneButton");
            var flyout = (MenuFlyout)button.Flyout;
            flyout.ShowAt(button);
            var choice = flyout.Items.OfType<MenuFlyoutItem>().Single(item => item.Tag as string == template);
            await Eventually(() => choice.IsLoaded);
            ((IInvokeProvider)new MenuFlyoutItemAutomationPeer(choice).GetPattern(PatternInterface.Invoke)).Invoke();
            return await OpenDialog(page.XamlRoot);
        }

        var dialog = await OpenCreate();
        Require(studio.Scenes.Count == count, "Opening the naming dialog created a scene before confirmation");
        Invoke(DialogButton(dialog, "CloseButton"));
        await Eventually(() => FindDialog(page.XamlRoot) is null);
        Require(studio.Scenes.Count == count && studio.PreviewSceneId == previewId && studio.ActiveSceneId == programId,
            "Cancelling new scene changed the show");
        dialog = await OpenCreate();
        var field = Descendants(dialog).OfType<TextBox>().Single(text => text.Name == "SceneNameField");
        foreach (var invalid in new[] { "   ", studio.ProgramScene.Name.ToUpperInvariant() })
        {
            field.Text = invalid;
            Invoke(DialogButton(dialog, "PrimaryButton"));
            await Task.Delay(60);
            Require(FindDialog(page.XamlRoot) is not null && studio.Scenes.Count == count &&
                Descendants(dialog).OfType<TextBlock>().Any(text => text.Name == "SceneNameError" && text.Text.Length > 0),
                $"Invalid scene name closed the dialog, created a scene, or omitted feedback: input='{invalid}', visible='{field.Text}', open={FindDialog(page.XamlRoot) is not null}, count={studio.Scenes.Count}/{count}, errors=[{string.Join(";", Descendants(dialog).OfType<TextBlock>().Where(text => text.Name == "SceneNameError").Select(text => text.Text))}]");
        }
        field.Text = "  Evening intro – 東京  ";
        if (reviewDirectory is not null)
            await ReviewImageWriter.SaveAsync(dialog, Path.Combine(reviewDirectory, "scene-naming-dialog.png"));
        Invoke(DialogButton(dialog, "PrimaryButton"));
        await Eventually(() => FindDialog(page.XamlRoot) is null);
        var namedId = studio.PreviewSceneId;
        Require(studio.PreviewScene.Name == "Evening intro – 東京" && studio.ActiveSceneId == programId,
            "Creating a named scene lost the trimmed name or changed Program");
        Require(store.Value.CustomScenes.Single(scene => scene.Id == namedId).Routes.Single().ShowInputSlotNumber == 1,
            "Named creation changed the starter's source assignment");
        checks.Add("compiled New scene chooser validates blank/duplicate names, cancels without mutation and creates a named Unicode scene on Preview");

        // #1197 separately tracks queued editor hydration changing the initial
        // route mode before any rename. Qualify rename from a loaded, saved
        // editor baseline, while retaining that earlier diagnostic failure.
        await Eventually(() => page.Workspace!.Items.Any(item => item.ItemId == studio.WorkspaceRoutes()[0].Id));
        await Task.Delay(250);
        page.Workspace!.SaveCommand.Execute(null);
        var routesBeforeRename = System.Text.Json.JsonSerializer.Serialize(studio.WorkspaceRoutes());
        var draftStatusBeforeRename = page.Workspace.DraftStatus;
        Require(draftStatusBeforeRename == "PREVIEW · Saved", "Rename fixture did not start from a saved scene");
        Invoke((Button)page.FindName("RenameSceneButton"));
        dialog = await OpenDialog(page.XamlRoot);
        field = Descendants(dialog).OfType<TextBox>().Single(text => text.Name == "SceneNameField");
        Require(field.Text == "Evening intro – 東京", "Rename did not start from the current scene name");
        field.Text = "Do not save this name";
        Invoke(DialogButton(dialog, "CloseButton"));
        await Eventually(() => FindDialog(page.XamlRoot) is null);
        Require(studio.PreviewScene.Name == "Evening intro – 東京", "Cancel renamed the scene");
        Invoke((Button)page.FindName("RenameSceneButton"));
        dialog = await OpenDialog(page.XamlRoot);
        field = Descendants(dialog).OfType<TextBox>().Single(text => text.Name == "SceneNameField");
        field.Text = "Opening interview";
        Invoke(DialogButton(dialog, "PrimaryButton"));
        await Eventually(() => FindDialog(page.XamlRoot) is null);
        Require(studio.PreviewSceneId == namedId && studio.ActiveSceneId == programId &&
            studio.SceneItems.Single(item => item.Id == namedId).Name == "Opening interview",
            "Renaming changed scene identity/selection or left the list stale");
        Require(System.Text.Json.JsonSerializer.Serialize(studio.WorkspaceRoutes()) == routesBeforeRename &&
            page.Workspace!.DraftStatus == draftStatusBeforeRename,
            $"Renaming changed ordinary scene routes or the draft's saved/unsaved state: status '{draftStatusBeforeRename}' -> '{page.Workspace!.DraftStatus}'; routes before={routesBeforeRename}; after={System.Text.Json.JsonSerializer.Serialize(studio.WorkspaceRoutes())}");
        if (reviewDirectory is not null)
            await ReviewImageWriter.SaveAsync(page, Path.Combine(reviewDirectory, "scene-naming-builder.png"));

        var builderTargetId = studio.Scenes.First(scene => !scene.Id.StartsWith("custom-", StringComparison.Ordinal)).Id;
        Grid? builderRow = null;
        await Eventually(() => (builderRow = Descendants(page).OfType<Grid>().FirstOrDefault(grid =>
            grid.DataContext is SceneDisplayItem item && item.Id == builderTargetId && grid.ContextFlyout is MenuFlyout)) is not null);
        var builderMenu = (MenuFlyout)builderRow!.ContextFlyout;
        builderMenu.ShowAt(builderRow);
        var builderRename = builderMenu.Items.OfType<MenuFlyoutItem>().Single(item => item.Text == "Rename scene");
        await Eventually(() => builderRename.IsLoaded);
        ((IInvokeProvider)new MenuFlyoutItemAutomationPeer(builderRename).GetPattern(PatternInterface.Invoke)).Invoke();
        dialog = await OpenDialog(page.XamlRoot);
        Descendants(dialog).OfType<TextBox>().Single(text => text.Name == "SceneNameField").Text = "Builder opening desk";
        Invoke(DialogButton(dialog, "PrimaryButton"));
        await Eventually(() => FindDialog(page.XamlRoot) is null);
        Require(studio.Scenes.Single(scene => scene.Id == builderTargetId).Name == "Builder opening desk" &&
            studio.PreviewSceneId == namedId && studio.PreviewScene.Name == "Opening interview" && studio.ActiveSceneId == programId,
            "Builder context rename targeted the selected scene instead of its row or changed Preview/Program");
        checks.Add("compiled Builder context menu renames its own row while preserving the selected Preview and on-air scene");

        var beforeTilesCount = studio.Scenes.Count;
        dialog = await OpenCreate("tiles");
        Require(Descendants(dialog).OfType<TextBox>().Single(text => text.Name == "SceneNameField").Text == "CoreVideo Tiles",
            "Tiles shortcut suggested a fixed-layout scene name");
        Invoke(DialogButton(dialog, "CloseButton"));
        await Eventually(() => FindDialog(page.XamlRoot) is null);
        Require(studio.Scenes.Count == beforeTilesCount && studio.PreviewSceneId == namedId && studio.ActiveSceneId == programId,
            "Cancelling the Tiles shortcut changed the show");
        dialog = await OpenCreate("tiles");
        Descendants(dialog).OfType<TextBox>().Single(text => text.Name == "SceneNameField").Text = "  Audience wall  ";
        Invoke(DialogButton(dialog, "PrimaryButton"));
        await Eventually(() => FindDialog(page.XamlRoot) is null && page.Workspace!.Items.Any(item => item.IsTilesWall));
        var wallId = studio.PreviewSceneId;
        Require(studio.Scenes.Count == beforeTilesCount + 1 && studio.ActiveSceneId == programId &&
            studio.PreviewScene is { Name: "Audience wall", Layout: "dynamic-gallery", DynamicGallery.AutoFill: true } &&
            page.Workspace!.Items.Count == 1 && page.Workspace.Items[0].IsTilesWall,
            "Tiles shortcut failed to create one editable auto-reflow wall on Preview without changing Program");
        Require(store.Value.CustomScenes.Single(scene => scene.Id == wallId) is
            { Name: "Audience wall", Layout: "dynamic-gallery", DynamicGallery.MembershipMode: "eligible", Routes.Count: 0 },
            "Tiles shortcut did not persist the named wall or added duplicate participant routes");
        studio.SelectSceneCommand.Execute(namedId);
        studio.SelectSceneCommand.Execute(wallId);
        await Eventually(() => page.Workspace!.Items.Any(item => item.IsTilesWall));
        Require(studio.PreviewScene.Name == "Audience wall" && studio.ActiveSceneId == programId,
            "Created Tiles scene could not be selected again without changing Program");
        var inspector = (Pivot)page.FindName("InspectorTabs");
        await Eventually(() => inspector.Items.Cast<PivotItem>().Select(item => item.Header?.ToString())
            .SequenceEqual(new[] { "Layout", "Style", "Members", "Transform" }));
        if (reviewDirectory is not null)
            await ReviewImageWriter.SaveAsync(page, Path.Combine(reviewDirectory, "scene-tiles-shortcut.png"));
        checks.Add("compiled Tiles starter supports naming and Cancel, creates one core-owned wall on Preview, exposes wall controls, persists and can be selected again");
        studio.ActiveSceneId = wallId;
        ((ITransportHost)studio).RefreshPreviewRoutingState();
        var liveGutter = studio.ProgramScene.DynamicGallery!.GutterPercent;
        studio.GalleryGutterPercent = 2.25;
        page.Workspace!.SaveCommand.Execute(null);
        studio.GalleryGutterPercent = 4.75; // unsaved draft, distinct from both stored and live values
        var ids = studio.Scenes.Select(scene => scene.Id).ToArray();
        Require(studio.TryRenameScene(wallId, "Panel after break", out var renameError), renameError ?? "Rename failed");
        Require(studio.Scenes.Select(scene => scene.Id).SequenceEqual(ids) && studio.PreviewSceneId == wallId && studio.ActiveSceneId == wallId,
            "Rename reordered scenes, changed an ID or cued/took another scene");
        Require(studio.ProgramScene.DynamicGallery.GutterPercent == liveGutter && studio.GalleryGutterPercent == 4.75,
            "Naming published the Preview draft or discarded unsaved content");
        Require(store.Value.CustomScenes.Single(scene => scene.Id == wallId) is { Name: "Panel after break", DynamicGallery.GutterPercent: 2.25 },
            "Naming discarded the saved draft or persisted unsaved content");
        Require(studio.ProgramSceneSummary == "Panel after break" && studio.PreviewSceneSummary == "Panel after break",
            "On-air/Preview scene labels stayed stale after rename");
        Require(!studio.TryRenameScene(wallId, "   ", out _) && !studio.TryRenameScene(wallId, "OPENING INTERVIEW", out _),
            "Rename accepted an empty or case-insensitive duplicate name");
        Require(!studio.TryRenameScene("missing-scene", "Missing", out _), "Stale rename target mutated another scene");
        studio.SelectSceneCommand.Execute(namedId);
        studio.ActiveTab = StudioTab.Studio;
        studio.SelectSceneCommand.Execute(wallId);
        Require(studio.PreviewScene.Name == "Panel after break" && studio.GalleryGutterPercent == 2.25,
            "Re-cue restored an old name or the wrong saved draft");
        checks.Add("scene rename preserves IDs/order, on-air contents, unsaved Preview edits and saved draft metadata; invalid/stale targets are rejected");

        // The Studio context menu must target its own row without cueing that scene.
        studio.SelectSceneCommand.Execute(namedId);
        var stage = new StudioWorkspace { ViewModel = studio };
        window.Content = stage;
        await Eventually(() => stage.IsLoaded);
        var catalogId = studio.Scenes.First(scene => !scene.Id.StartsWith("custom-", StringComparison.Ordinal)).Id;
        Button? sceneButton = null;
        await Eventually(() => (sceneButton = Descendants(stage).OfType<Button>().FirstOrDefault(button =>
            button.CommandParameter as string == catalogId && button.ContextFlyout is MenuFlyout)) is not null);
        var renameMenu = (MenuFlyout)sceneButton!.ContextFlyout;
        renameMenu.ShowAt(sceneButton);
        var rename = renameMenu.Items.OfType<MenuFlyoutItem>().Single(item => item.Text == "Rename scene");
        await Eventually(() => rename.IsLoaded);
        ((IInvokeProvider)new MenuFlyoutItemAutomationPeer(rename).GetPattern(PatternInterface.Invoke)).Invoke();
        dialog = await OpenDialog(stage.XamlRoot);
        Descendants(dialog).OfType<TextBox>().Single(text => text.Name == "SceneNameField").Text = "Welcome desk";
        Invoke(DialogButton(dialog, "PrimaryButton"));
        await Eventually(() => FindDialog(stage.XamlRoot) is null);
        Require(studio.Scenes.Single(scene => scene.Id == catalogId).Name == "Welcome desk" &&
            studio.PreviewSceneId == namedId && studio.ActiveSceneId == wallId,
            "Studio rename targeted the selected scene instead of its row or changed Preview/Program");
        studio.NewSceneCommand.Execute("closing"); // another full preferences save
        Require(store.Value.SceneNames[catalogId] == "Welcome desk" &&
            store.Value.CustomScenes.Single(scene => scene.Id == wallId).Name == "Panel after break",
            "A subsequent preferences save restored an old scene name");
        await using var reloaded = new StudioViewModel(StudioStartupBootstrap.Create(store, startInteractiveServices: false));
        Require(reloaded.Scenes.Single(scene => scene.Id == catalogId).Name == "Welcome desk" &&
            reloaded.Scenes.Single(scene => scene.Id == namedId).Name == "Opening interview" &&
            reloaded.Scenes.Single(scene => scene.Id == wallId).Name == "Panel after break" &&
            reloaded.Scenes.Single(scene => scene.Id == wallId).DynamicGallery!.GutterPercent == 2.25,
            "Restart lost a catalog/custom name, saved content, or scene identity");
        checks.Add("compiled Studio row rename preserves cue/on-air identity; starter and custom names survive a later save and a fresh application model");
        var otherCatalog = reloaded.Scenes.First(scene => scene.Id != catalogId && !scene.Id.StartsWith("custom-", StringComparison.Ordinal));
        var otherName = otherCatalog.Name;
        Require(reloaded.TryRenameScene(catalogId, "Temporary swap name", out _) &&
            reloaded.TryRenameScene(otherCatalog.Id, "Welcome desk", out _) &&
            reloaded.TryRenameScene(catalogId, otherName, out _), "Valid name swap was rejected");
        await using var swapped = new StudioViewModel(StudioStartupBootstrap.Create(store, startInteractiveServices: false));
        Require(swapped.Scenes.Single(scene => scene.Id == catalogId).Name == otherName &&
            swapped.Scenes.Single(scene => scene.Id == otherCatalog.Id).Name == "Welcome desk",
            "Restoring a valid name swap collided with the starter names");
        checks.Add("renamed starter scenes can swap names without a restore-order collision");
    }

    private static ContentDialog? FindDialog(XamlRoot root) => VisualTreeHelper.GetOpenPopupsForXamlRoot(root)
        .Where(popup => popup.IsOpen).Select(popup => popup.Child).Where(child => child is not null)
        .SelectMany(child => Descendants(child).Prepend(child)).OfType<ContentDialog>().FirstOrDefault();
    private static async Task<ContentDialog> OpenDialog(XamlRoot root)
    {
        ContentDialog? dialog = null;
        await Eventually(() => (dialog = FindDialog(root)) is { IsLoaded: true });
        return dialog!;
    }
    private static Button DialogButton(ContentDialog dialog, string name) => Descendants(dialog).OfType<Button>().Single(button => button.Name == name);
    private static void Invoke(Button button) => ((IInvokeProvider)new ButtonAutomationPeer(button).GetPattern(PatternInterface.Invoke)).Invoke();
    private static async Task Eventually(Func<bool> ready)
    {
        var watch = Stopwatch.StartNew();
        while (!ready()) { if (watch.Elapsed > TimeSpan.FromSeconds(10)) throw new InvalidOperationException("Scene naming UI probe timed out"); await Task.Delay(20); }
    }
    private static void Require(bool condition, string message) { if (!condition) throw new InvalidOperationException(message); }
    private static IEnumerable<DependencyObject> Descendants(DependencyObject root)
    {
        for (var i = 0; i < VisualTreeHelper.GetChildrenCount(root); i++)
        {
            var child = VisualTreeHelper.GetChild(root, i); yield return child;
            foreach (var nested in Descendants(child)) yield return nested;
        }
    }
}
