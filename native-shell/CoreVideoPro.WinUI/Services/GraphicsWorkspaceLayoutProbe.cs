using System.Diagnostics;
using CoreVideoPro.MediaCore.Models;
using CoreVideoPro.MediaCore.Services;
using CoreVideoPro.WinUI.Controls;
using CoreVideoPro.WinUI.ViewModels;
using CoreVideoPro.WinUI.Views;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Automation.Peers;
using Microsoft.UI.Xaml.Automation.Provider;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Media;
using Windows.Graphics;

namespace CoreVideoPro.WinUI.Services;

// Compiled operator controls at the usable 1080p shell content budget. No core,
// camera, meeting or production preference store. Swapchain video is not sampled.
internal static class GraphicsWorkspaceLayoutProbe
{
    internal static async Task CheckAsync(Window window, List<string> checks, string directory)
    {
        var previousPath = Environment.GetEnvironmentVariable("COREVIDEO_GRAPHICS_DOCUMENT_PATH");
        Environment.SetEnvironmentVariable("COREVIDEO_GRAPHICS_DOCUMENT_PATH", Path.Combine(directory, "isolated-graphics-" + Guid.NewGuid().ToString("N") + ".json"));
        try {
            await using var studio = new StudioViewModel(StudioStartupBootstrap.Create(new InMemoryProductionOutputPreferencesStore(), startInteractiveServices: false));
            var page = new OverlaysPage { ViewModel = studio, RequestedTheme = ElementTheme.Dark };
            window.Content = page;
            window.AppWindow.MoveAndResize(new RectInt32(-20000, -20000, 1920, 1080));
            window.AppWindow.Show(false);
            await Eventually(() => page.IsLoaded);
            // #1076: Playout is the default mode; the strip and playlist mount above/instead of the
            // workspace, which stays in the tree (exactly one) with Visibility toggled.
            Require(studio.IsGraphicsPlayoutMode && Descendants(page).OfType<GraphicsPlayoutControl>().Single().Visibility == Visibility.Visible, "Graphics did not open in Playout mode");
            Require(Descendants(page).OfType<GraphicsOnAirStripControl>().Any(), "On Air strip is missing from the Graphics tab");
            Require(Descendants(page).OfType<GraphicsWorkspaceControl>().Single().Visibility == Visibility.Collapsed, "Design workspace is visible in Playout mode");
            // Graphics Designer slice 1: one stored design (document only; no core needed to list it).
            var seedManifest = GraphicsDesignSeeds.Manifest("lower-third", "design-probe-strap-a1b2c3", 1, "Probe strap");
            var seedDefinition = System.Text.Json.JsonSerializer.SerializeToElement(seedManifest);
            Require(await studio.GraphicsLibrary.SaveDesignsAsync([new GraphicsDesign("design-probe-strap-a1b2c3", "Probe strap", "lower-third", "design-probe-strap-a1b2c3", 1, seedDefinition, [], DateTimeOffset.UtcNow)], "Probe design stored."), "Probe design could not be stored");
            Invoke(Button(page, "Design")); await Task.Delay(80); page.UpdateLayout();
            // #1076 slice 3: Design opens on the template-first builder; the old workspace sits behind "Cues & automation".
            Require(studio.IsGraphicsDesignMode && Descendants(page).OfType<GraphicsBuilderControl>().Single().Visibility == Visibility.Visible, "Design toggle did not reveal the builder");
            Require(Descendants(page).OfType<GraphicsWorkspaceControl>().Single().Visibility == Visibility.Collapsed, "The Cues & automation workspace is visible before it is chosen");
            Require(Descendants(page).OfType<Microsoft.UI.Xaml.Controls.Button>().Count(b => b.Tag is GraphicsBuilderKind) == GraphicsBuilder.Kinds.Count, "Builder does not offer every kind of graphic");
            await Eventually(() => Descendants(page).OfType<Button>().Any(b => b.Content?.ToString() == "Probe strap"), "Design does not list the stored design under Your designs");
            Require(Descendants(page).OfType<TextBlock>().Any(t => t.Text == "YOUR DESIGNS" && t.ActualHeight > 0), "Design does not show Your designs");
            foreach (var action in new[] { "New design", "Open design", "Duplicate design", "Delete design" })
                Require(Button(page, action).ActualHeight > 0, action + " is missing from Your designs");
            var builderCanvas = Descendants(page).OfType<GraphicsBuilderControl>().Single();
            var previewCanvas = Descendants(builderCanvas).OfType<GraphicsAuthoringCanvas>().Single();
            foreach (var (width, height, suffix, minimum) in new[] {
                (1320d, 640d, "1360x768", 630d), (1560d, 772d, "1600x900", 840d), (1840d, 820d, "1080p", 950d) })
            {
                page.Width = width;
                page.Height = height;
                await Task.Delay(100);
                page.UpdateLayout();
                var preview = previewCanvas.PreviewViewport;
                Require(preview.Width >= minimum && Math.Abs(preview.Width / preview.Height - 16d / 9) < .01,
                    $"Builder preview at {suffix} is only {preview.Width:0}x{preview.Height:0}.");
                var save = Button(builderCanvas, "Add to playlist");
                var savePosition = save.TransformToVisual(builderCanvas).TransformPoint(new(0, 0));
                Require(save.ActualHeight > 0 && savePosition.Y >= 0 &&
                    savePosition.Y + save.ActualHeight <= builderCanvas.ActualHeight + 1,
                    $"Builder Add to playlist is outside the visible controls at {suffix}.");
                checks.Add($"Builder preview at {suffix} fits at {preview.Width:0}x{preview.Height:0}.");
                await ReviewImageWriter.SaveAsync(page, Path.Combine(directory, $"graphics-builder-{suffix}.png"));
            }
            page.ClearValue(FrameworkElement.WidthProperty);
            page.ClearValue(FrameworkElement.HeightProperty);
            await Task.Delay(100);
            page.UpdateLayout();
            await ReviewImageWriter.SaveAsync(page, Path.Combine(directory, "graphics-builder.png"));
            Invoke(Button(page, "New design"));
            GraphicsDesignStartDialog? startDialog = null;
            await Eventually(() => (startDialog = VisualTreeHelper.GetOpenPopupsForXamlRoot(page.XamlRoot).Select(p => p.Child).Where(c => c is not null)
                .SelectMany(c => Descendants(c).Prepend(c)).OfType<GraphicsDesignStartDialog>().FirstOrDefault()) is { IsLoaded: true }, "New design did not open the start dialog");
            var blankKinds = Descendants(startDialog!).OfType<Microsoft.UI.Xaml.Controls.Primitives.ToggleButton>().Where(b => b.Tag is string kind && GraphicsDesignSeeds.Kinds.Contains(kind)).Select(b => (string)b.Tag).ToArray();
            Require(blankKinds.SequenceEqual(GraphicsDesignSeeds.Kinds), "Start dialog does not list the four blank kinds: " + string.Join(",", blankKinds));
            Require(Descendants(startDialog!).OfType<TextBox>().Any(t => t.Header as string == "Name"), "Start dialog has no name field");
            Require(Descendants(startDialog!).OfType<ComboBox>().Any(c => c.Header as string == "Or start from a look"), "Start dialog does not offer starting from a look");
            Require(!startDialog!.IsPrimaryButtonEnabled, "Open in the designer is enabled before a start and a name are chosen");
            Descendants(startDialog!).OfType<Microsoft.UI.Xaml.Controls.Primitives.ToggleButton>().First(b => b.Tag as string == "lower-third").IsChecked = true;
            Descendants(startDialog!).OfType<TextBox>().Single(t => t.Header as string == "Name").Text = "Probe lower third";
            await Eventually(() => startDialog!.IsPrimaryButtonEnabled, "Choosing a blank kind and a name did not enable Open in the designer");
            await ReviewImageWriter.SaveAsync(startDialog!, Path.Combine(directory, "graphics-design-start-dialog.png"));
            startDialog!.Hide(); await Task.Delay(150);
            checks.Add("Design lists Your designs (Open / Duplicate / Delete) and New design opens the start dialog with the four blank kinds, a look picker and a name");
            Invoke(Button(page, "Cues & automation")); await Task.Delay(80); page.UpdateLayout();
            Require(studio.GraphicsDesignAdvanced && Descendants(page).OfType<GraphicsWorkspaceControl>().Single().Visibility == Visibility.Visible, "Cues & automation did not reveal the workspace");
            // Graphics Designer slice 2: the Designer is the editor; the workspace keeps no layer pane and no design actions.
            var cuesWorkspace = Descendants(page).OfType<GraphicsWorkspaceControl>().Single();
            Require(!Descendants(cuesWorkspace).OfType<Button>().Any(b => b.Content?.ToString() is "Edit layers ↗" or "Save design" || Microsoft.UI.Xaml.Automation.AutomationProperties.GetName(b) is "Save design" or "Save to playlist"),
                "Cues & automation still offers Edit layers or the design save actions");
            checks.Add("Graphics opens in Playout (strip + playlist); Design reveals the builder with every kind; Cues & automation reveals the single existing workspace control, with no Edit layers or design save actions");
            var backToBuilder = Button(cuesWorkspace, "← Build a graphic");
            Require(backToBuilder.ActualHeight > 0 && backToBuilder.IsEnabled, "Cues & automation has no visible way back to the builder");
            Invoke(backToBuilder); await Task.Delay(80); page.UpdateLayout();
            Require(!studio.GraphicsDesignAdvanced && Descendants(page).OfType<GraphicsBuilderControl>().Single().Visibility == Visibility.Visible && cuesWorkspace.Visibility == Visibility.Collapsed,
                "← Build a graphic did not return from Cues & automation to the builder");
            Invoke(Button(page, "Cues & automation")); await Task.Delay(80); page.UpdateLayout();
            Require(studio.GraphicsDesignAdvanced && cuesWorkspace.Visibility == Visibility.Visible, "Cues & automation did not reopen after returning to the builder");
            checks.Add("← Build a graphic (Cues & automation header) returns to the builder");
            // Reserve the existing navigation/header/production footer rather
            // than pretending Graphics can consume the entire display.
            page.Width = 1840; page.Height = 820; page.HorizontalAlignment = HorizontalAlignment.Left; page.VerticalAlignment = VerticalAlignment.Top;
            var template = new NativeGraphicsTemplate { TemplateId="compact-solid", TemplateVersion=1, Name="Compact solid", Family="lower-third", Runtime="compatibility" };
            var logoFixture = System.Text.Json.JsonSerializer.Deserialize<NativeGraphicsTemplate>("""{"defaultPlacement":{"anchor":"upper-right","scale":1,"x":-64,"y":48},"definition":{"animations":{"in":{"durationMs":250,"easing":"ease-out","type":"fade"},"out":{"durationMs":200,"easing":"ease-in","type":"fade"}},"assets":[{"file":"assets/5c998247459cadf4c7229c685d5ca8257e92e1154a19767a4be874c69def0d09.png","id":"logo","sha256":"5c998247459cadf4c7229c685d5ca8257e92e1154a19767a4be874c69def0d09"}],"bounds":{"height":192,"width":192},"brandTokens":["brand.primary","brand.onPrimary","brand.onPrimaryMuted","brand.fontBody","brand.accent"],"canvas":{"height":1080,"width":1920},"defaultPlacement":{"anchor":"upper-right","scale":1,"x":-64,"y":48},"family":"bug","fields":[{"default":"logo","id":"logo","type":"image"},{"default":true,"id":"show-logo","type":"boolean"}],"layers":[{"align":"center","asset":{"field":"logo"},"fit":"contain","height":168,"id":"logo-image","type":"image","verticalAlign":"center","visible":{"field":"show-logo"},"width":168,"x":12,"y":12}],"name":"Corner logo bug","schemaVersion":1,"templateId":"corner-logo","templateVersion":1},"family":"bug","fields":[{"default":"logo","id":"logo","type":"image"},{"default":true,"id":"show-logo","type":"boolean"}],"layers":[{"align":"center","asset":{"field":"logo"},"fit":"contain","height":168,"id":"logo-image","type":"image","verticalAlign":"center","visible":{"field":"show-logo"},"width":168,"x":12,"y":12}],"manifestHash":"5e2a31231137a532fe771b84a56efa4e0fe9eb9f23c36975ce5ef177c9cd63b2","name":"Corner logo bug","runtime":"declarative","templateId":"corner-logo","templateVersion":1}""",new System.Text.Json.JsonSerializerOptions(System.Text.Json.JsonSerializerDefaults.Web))!;
            var catalog = new[] { template, template with { TemplateId="minimal-accent",Name="Minimal accent" }, template with { TemplateId="broadcast",Name="Broadcast" },
                new NativeGraphicsTemplate {TemplateId="centered-title",TemplateVersion=1,Name="Centered title",Family="title",Runtime="declarative",Fields=[new() {Id="heading",Type="string",Default=System.Text.Json.JsonSerializer.SerializeToElement("Welcome")},new() {Id="subtitle",Type="string",Default=System.Text.Json.JsonSerializer.SerializeToElement("Your show")}]},
                logoFixture,
                new NativeGraphicsTemplate {TemplateId="clock-panel",TemplateVersion=1,Name="Clock",Family="clock",Runtime="declarative",Fields=[new() {Id="label",Type="string",Default=System.Text.Json.JsonSerializer.SerializeToElement("LIVE")},new() {Id="time",Type="string",Default=System.Text.Json.JsonSerializer.SerializeToElement("12:34:56")}]} };
            // Optional catalog collected from a separate native QA process.
            // This opt-in probe still never attaches to the running application.
            if(Environment.GetEnvironmentVariable("COREVIDEO_GRAPHICS_PROBE_CATALOG") is {Length:>0} catalogPath)
                catalog=System.Text.Json.JsonSerializer.Deserialize<NativeGraphicsState>(await File.ReadAllTextAsync(catalogPath),new System.Text.Json.JsonSerializerOptions(System.Text.Json.JsonSerializerDefaults.Web))!.Templates.ToArray();
            studio.GraphicsWorkspace.Observe(new() {CoreEpoch="isolated-layout",SchemaVersion=1,Templates=catalog});
            await Task.Delay(150);page.UpdateLayout();
            var workspace = Descendants(page).OfType<GraphicsWorkspaceControl>().Single();
            await Eventually(()=>Descendants(workspace).OfType<Image>().Count(i=>Microsoft.UI.Xaml.Automation.AutomationProperties.GetName(i).StartsWith("Show template reference: ",StringComparison.Ordinal) && i.Source is Microsoft.UI.Xaml.Media.Imaging.BitmapImage {PixelWidth:320})==4);
            foreach(var name in GraphicsShowStarters.All.Select(s=>s.Name)) {
                var card=Button(workspace,name);var point=card.TransformToVisual(workspace).TransformPoint(new(0,0));
                Require(card.ActualHeight>100 && point.Y>=0 && point.Y+card.ActualHeight<=workspace.ActualHeight+1,"Show template card is clipped: "+name);
            }
            await ReviewImageWriter.SaveAsync(page,Path.Combine(directory,"graphics-show-templates-1080.png"));
            Invoke(Button(workspace,"Interview"));await Task.Delay(100);page.UpdateLayout();
            Require(studio.GraphicsWorkspace.Selected is null && studio.GraphicsWorkspace.Preview?.Fields.GetProperty("name").GetString()=="Alex Rivera","Show template did not provide private sample content");
            Require(studio.GraphicsWorkspace.State?.Instances.Count==0 && studio.GraphicsLibrary.Document.StudioLowerThird is null,"Trying a template mutated the show or Studio assignment");
            Require(Button(workspace,"Save graphics only").ActualHeight>0 && Button(workspace,"Use this look in Studio").IsEnabled,"The direct sample-to-Studio action is unavailable");
            var sampleList=Descendants(workspace).OfType<ListView>().Single(l=>Microsoft.UI.Xaml.Automation.AutomationProperties.GetName(l)=="Show template sample graphics");
            sampleList.SelectedIndex=1;
            var heading = Descendants(workspace).OfType<TextBox>().Single(t=>t.Text=="In conversation");
            Require(heading.FocusState==FocusState.Unfocused,"Pending-edit regression requires an unfocused editor");
            heading.Text="My weekly show";
            // Force the real refresh path before asynchronous TextChanged. A
            // model echo must not overwrite input waiting to reach the model.
            studio.GraphicsWorkspace.SetCompare(true);
            await Eventually(()=>studio.GraphicsWorkspace.Draft?.Fields.GetProperty("heading").GetString()=="My weekly show", "Pending Graphics heading was overwritten before commit");
            sampleList.SelectedIndex=0;
            var currentFields = studio.GraphicsWorkspace.Draft!.Fields.GetRawText();
            heading.Text="Late event from a retired editor";
            await Task.Delay(50);
            Require(studio.GraphicsWorkspace.Draft.Fields.GetRawText()==currentFields,"Retired Graphics editor changed the new sample");
            checks.Add("pending text survives model refresh and retired text callbacks cannot alter the next sample");
            Descendants(workspace).OfType<TextBox>().Single(t=>t.Header as string=="Brand color").Text="#2873AA";
            await Eventually(()=>studio.GraphicsWorkspace.Draft?.Style.AccentColor=="#2873AA");
            Require(studio.GraphicsWorkspace.Draft?.Style.AccentColor=="#2873AA","Shared branding did not update the private draft");
            studio.GraphicsWorkspace.SetField("name",System.Text.Json.JsonSerializer.SerializeToElement("My guest"));
            sampleList.SelectedIndex=1;
            Require(studio.GraphicsWorkspace.Draft?.Fields.GetProperty("heading").GetString()=="My weekly show","Edited welcome title was lost while recoloring or choosing another graphic");
            sampleList.SelectedIndex=0;
            Require(studio.GraphicsWorkspace.Draft?.Fields.GetProperty("name").GetString()=="My guest","Switching sample graphics lost private edits");
            var sourceText = Descendants(workspace).OfType<CheckBox>().Single(c => c.Content as string == "Use the Program source name and title in Studio");
            sourceText.IsChecked = false;
            Require(Descendants(workspace).OfType<TextBlock>().Any(t => t.Text.Contains("including the sample name")), "Fixed sample text has no Studio warning");
            sourceText.IsChecked = true;
            Invoke(Button(workspace,"Edit source names in Sources →"));
            Require(studio.ActiveTab == CoreVideoPro.WinUI.Models.StudioTab.Sources, "Guest text shortcut did not navigate to existing Sources");
            Require(studio.GraphicsWorkspace.Draft?.Fields.GetProperty("name").GetString()=="My guest", "Guest text handoff lost the private sample draft");
            studio.ActiveTab = CoreVideoPro.WinUI.Models.StudioTab.Overlays;
            checks.Add("Studio text choice explains sample versus Program guest text; Sources handoff retains private draft and never assigns or shows it");
            await ReviewImageWriter.SaveAsync(page,Path.Combine(directory,"graphics-try-interview-1080.png"));
            checks.Add("Four visible show-template starters; Interview opens populated private samples; branding and per-graphic edits survive navigation without creating native instances or assigning Studio");
            Invoke(Button(workspace,"Show templates"));Invoke(Button(workspace,"Browse individual graphics"));await Task.Delay(80);
            var individualLibrary=Descendants(workspace).OfType<ListView>().Single(l=>l.IsItemClickEnabled);
            individualLibrary.SelectedIndex=0;Invoke(Button(workspace,"Close"));await Task.Delay(80);
            Require(sampleList.Visibility==Visibility.Collapsed && studio.GraphicsWorkspace.Selected is null && studio.GraphicsWorkspace.Draft?.TemplateId==catalog[0].TemplateId,"Individual library browsing retained the show-starter mode or hid the editor");
            checks.Add("Returning to individual graphics from a starter exits sample-set mode and preserves the original library workflow");
            studio.GraphicsWorkspace.Browse(catalog.Single(t=>t.TemplateId=="corner-logo"));
            await Task.Delay(100); Invoke(Button(workspace,"Customize layout & motion")); Invoke(Button(workspace,"Content")); await Task.Delay(80);page.UpdateLayout();
            Require(Button(workspace,"Upload logo PNG / JPEG…").IsEnabled && Button(workspace,"Upload logo PNG / JPEG…").ActualHeight > 0,"Declarative logo uploader is missing or disabled");
            Require(Descendants(workspace).OfType<GraphicsImageControl>().Any(),"Logo template only offers bundled asset IDs");
            Require(Descendants(workspace).OfType<ComboBox>().Count(c=>c.Header as string == "Logo")==1,"Logo uploader is duplicated by an asset-ID-only selector");
            Require(studio.GraphicsWorkspace.Selected is null && studio.GraphicsLibrary.Document.StudioLowerThird is null,"Logo editing implicitly created or assigned a cue");
            checks.Add("Declarative logo template exposes an enabled PNG/JPEG uploader in Content without creating or showing a cue");
            Invoke(Button(workspace,"Use simple controls"));
            var seedTemplate = System.Text.Json.JsonSerializer.Deserialize<NativeGraphicsTemplate>(seedManifest.ToJsonString(),new System.Text.Json.JsonSerializerOptions(System.Text.Json.JsonSerializerDefaults.Web))! with {
                Runtime="declarative", Definition=seedDefinition, ManifestHash=NativeGraphicsScratch.From(seedDefinition).Hash };
            studio.GraphicsWorkspace.Observe(new() {CoreEpoch="isolated-layout",SchemaVersion=1,Templates=[..catalog,seedTemplate]});
            await CheckDesignerAsync(page, studio, seedTemplate, checks, directory);
            studio.GraphicsDesignAdvanced = true; await Task.Delay(80);
            page.Width = 1840; page.Height = 820; await Task.Delay(100); page.UpdateLayout();
            studio.GraphicsWorkspace.Observe(new() {CoreEpoch="isolated-layout",SchemaVersion=1,Templates=catalog});
            studio.GraphicsWorkspace.Browse(catalog[0]);
            var state = new NativeGraphicsState { CoreEpoch="isolated-layout", SchemaVersion=1, Templates=catalog,
                Instances=Enumerable.Range(1,8).Select(n=>new NativeGraphicsInstance { InstanceId=$"fixture-{n}", Label=n==1?"Guest introduction":$"Show cue {n}", TemplateId=template.TemplateId, TemplateVersion=1, Scope=n==8?"scene":"global", SceneId=n==8?"other-scene":"", Revision=1, Fields=new() { Name="Alex Rivera",Title="Producer · Office Hours" } }).ToArray() };
            studio.GraphicsWorkspace.Observe(state); studio.GraphicsWorkspace.Select("fixture-1");
            Invoke(Button(workspace,"Show templates")); Invoke(Button(workspace,"Continue my show"));
            await Task.Delay(250); page.UpdateLayout();
            Require(workspace.ActualHeight <= 821 && workspace.ActualWidth <= 1841, "Graphics exceeded the 1080p content budget");
            var canvas = Descendants(workspace).OfType<GraphicsAuthoringCanvas>().Single();
            await ReviewImageWriter.SaveAsync(page,Path.Combine(directory,"graphics-942-initial-1080.png"));
            Require(canvas.ActualWidth > 700 && canvas.ActualHeight > 350, $"Native canvas was crowded out: {canvas.ActualWidth} x {canvas.ActualHeight}; workspace {workspace.ActualWidth} x {workspace.ActualHeight}");
            foreach (var title in new[] {"Cue to Preview","Show on Program","Hide","Update on air","Cancel draft","Clear graphics","Use in Studio"}) {
                var button=Button(workspace,title); var point=button.TransformToVisual(workspace).TransformPoint(new(0,0));
                Require(button.ActualHeight>0 && point.Y>=0 && point.Y+button.ActualHeight<=workspace.ActualHeight+1 && point.X+button.ActualWidth<=workspace.ActualWidth+1, $"{title} is clipped or below the workspace");
            }
            checks.Add("1840 x 820 shell content budget on 1920 x 1080: native canvas, inspector and all fixed actions stay visible; eight cue rows bounded");
            Invoke(Button(workspace,"Customize layout & motion"));
            foreach(var tab in new[] {"Content","Style","Layout","Motion"}) {
                Invoke(Button(workspace,tab)); await Task.Delay(80); page.UpdateLayout();
                Require(Descendants(workspace).OfType<ScrollViewer>().Any(s=>s.Visibility==Visibility.Visible && s.ActualHeight>100), "Inspector has no bounded scroll area");
                await ReviewImageWriter.SaveAsync(page,Path.Combine(directory,$"graphics-942-{tab.ToLowerInvariant()}-1080.png"));
            }
            checks.Add("Content, Style, Layout and Motion switch independently; compiled-control screenshots exclude native video pixels");
            Invoke(Button(workspace,"Templates and saved cues")); await Task.Delay(80);
            Require(Button(workspace,"Import PNG / JPEG graphic…").IsEnabled,"Standalone transparent artwork import is unreachable");
            Invoke(Button(workspace,"Close"));
            checks.Add("Standalone PNG/JPEG graphic import is available in the library without changing the current cue");
            Invoke(Button(workspace,"Manage")); await Task.Delay(80);
            Invoke(Button(workspace,"Existing overlay controls")); await Task.Delay(80);
            var legacyDrawer=Descendants(page).OfType<SplitView>().Single(s=>s.Name=="ExistingOverlayDrawer");
            Require(legacyDrawer.IsPaneOpen,"Existing overlay controls are unreachable");
            Invoke(Button(page,"Close existing overlay controls"));
            Require(!legacyDrawer.IsPaneOpen,"Existing controls cannot be closed");
            Invoke(Button(workspace,"Automation")); await Task.Delay(80);
            Require(Descendants(workspace).OfType<GraphicsAutomationControl>().Any(),"Existing automation editor was lost");
            Invoke(Button(workspace,"Close"));
            checks.Add("Manage to existing overlays and Automation drawers open and close without replacing the workspace");
            Require(studio.GraphicsWorkspace.SelectedId=="fixture-1" && studio.GraphicsWorkspace.Draft?.Fields.GetProperty("name").GetString()=="Alex Rivera", "Navigation mutated the cue draft");
            Require(studio.GraphicsLibrary.Document.StudioLowerThird is null,"Rendering assigned a look implicitly");
            checks.Add("Tab/drawer navigation preserves selection and draft and never assigns a Studio look");
            var cueList=Descendants(workspace).OfType<ListView>().Single(l=>Microsoft.UI.Xaml.Automation.AutomationProperties.GetName(l)=="Graphics show cues");
            var search=Descendants(workspace).OfType<TextBox>().Single(t=>t.PlaceholderText=="Find a cue");
            // TextChanged / SelectionChanged filtering is asynchronous (#970): wait on the real
            // control state, bounded, and report the actual field, count and selection on failure.
            string CueState()=>$"search='{search.Text}', items={cueList.Items.Count}, selectedId='{studio.GraphicsWorkspace.SelectedId}'";
            search.Text="Show cue 2";
            await Eventually(()=>cueList.Items.Count==1,()=>"Cue filtering did not narrow to the matching cue ("+CueState()+")");
            Require(cueList.Items.Count==1 && studio.GraphicsWorkspace.SelectedId=="fixture-1","Cue filtering changed selection or returned the wrong cue ("+CueState()+")");
            search.Text=""; var scope=Descendants(workspace).OfType<ComboBox>().Single(c=>c.Header?.ToString()=="Scope"); scope.SelectedIndex=1;
            await Eventually(()=>cueList.Items.Count==7,()=>"Current Program scope did not retain global cues and hide the other scene ("+CueState()+")");
            Require(cueList.Items.Count==7,"Current Program scope did not retain global cues and hide the other scene ("+CueState()+")"); scope.SelectedIndex=0;
            await Eventually(()=>cueList.Items.Count==8,()=>"Clearing cue filters did not restore every cue ("+CueState()+")");
            Require(cueList.Items.Count==8 && studio.GraphicsWorkspace.SelectedId=="fixture-1","Clearing cue filters lost the selected cue ("+CueState()+")");
            checks.Add("Cue search and Program-scene filtering retain the selected private draft and restore all cue rows");
            using var control = new StudioControlSurface(studio, window.DispatcherQueue);
            var hidden = await control.InvokeAsync("graphics.lowerThird.set", new object?[] { false });
            Require(hidden.Ok,"Idempotent legacy Hide was rejected");
            Invoke(Button(workspace,"Use in Studio"));
            await Eventually(()=>studio.GraphicsLibrary.Document.StudioLowerThird is not null && !studio.GraphicsLibrary.Busy);
            Require(studio.StudioAssignedLowerThirdLabel=="Guest introduction","Assigned look label did not update after explicit assignment");
            studio.GraphicsWorkspace.Select("fixture-2");
            Require(studio.StudioAssignedLowerThirdLabel=="Guest introduction","Browsing changed the assigned look label");
            Invoke(Button(workspace,"Go to Studio"));
            Require(studio.ActiveTab==CoreVideoPro.WinUI.Models.StudioTab.Studio,"Go to Studio did not select the existing Studio workspace");
            checks.Add("Use in Studio saves the selected look and updates the bound Studio label; later cue selection preserves assignment");
            var unavailable = await control.InvokeAsync("graphics.lowerThird.set", new object?[] { true });
            Require(!unavailable.Ok && unavailable.Error?.Contains("Native Graphics is unavailable",StringComparison.Ordinal)==true,"Assigned API Show fell through to a legacy cue or falsely reported success");
            checks.Add("API Hide remains idempotent; assigned API Show refuses unavailable native Graphics without legacy fallback or false success");
            await CheckBusyTextBindingAsync(window, checks);
            await CheckAutomationEditingAsync(window, studio, checks);
        } finally { Environment.SetEnvironmentVariable("COREVIDEO_GRAPHICS_DOCUMENT_PATH",previousPath); }
    }
    /// <summary>Graphics Designer slice 2: the builder's "Open in Designer" with "Probe strap" selected (already the
    /// private draft) shows it in the Designer; then, at the 1360x768 and
    /// 1600x900 shell budgets (page 1320x640 and 1560x772): one surface, the seed's layer rows, every fixed action
    /// inside the designer, a canvas that is not crowded out, the Variables tab, and Add text / Undo.</summary>
    private static async Task CheckDesignerAsync(OverlaysPage page, StudioViewModel studio, NativeGraphicsTemplate seedTemplate, List<string> checks, string directory)
    {
        studio.GraphicsWorkspace.Browse(seedTemplate);
        studio.GraphicsDesignAdvanced = false; await Task.Delay(80); page.UpdateLayout();
        var builder = Descendants(page).OfType<GraphicsBuilderControl>().Single();
        Require(builder.Visibility == Visibility.Visible, "The builder is not shown before Open in Designer");
        Invoke(Button(builder, "Probe strap")); await Task.Delay(50);
        Invoke(Button(builder, "Open in Designer"));
        var designer = Descendants(page).OfType<GraphicsDesignerControl>().Single();
        await Eventually(() => studio.GraphicsDesignerOpen && designer.Visibility == Visibility.Visible, () => $"Open in Designer with Probe strap selected did not show the Designer (open={studio.GraphicsDesignerOpen}, surface={studio.GraphicsDesignSurface})");
        checks.Add("Builder: Open in Designer with the Probe strap design selected shows it in the Designer");
        IReadOnlyList<GraphicsDesignerRow> Rows() => Descendants(designer).OfType<FrameworkElement>().Select(e => e.Tag).OfType<GraphicsDesignerRow>().ToArray();
        foreach (var (width, height, shot, minCanvasWidth, minCanvasHeight) in new[] { (1320d, 640d, "graphics-designer-1360x768.png", 640d, 360d), (1560d, 772d, "graphics-designer-1600x900.png", 640d, 360d), (1840d, 820d, "graphics-designer-1080p.png", 640d, 360d) }) {
            page.Width = width; page.Height = height; await Task.Delay(150); page.UpdateLayout();
            await Eventually(() => designer.Visibility == Visibility.Visible && Rows().Count == 4, () => $"The Designer did not open on the Probe strap draft (visible={designer.Visibility}, rows={Rows().Count})");
            Require(Descendants(page).OfType<GraphicsBuilderControl>().Single().Visibility == Visibility.Collapsed && Descendants(page).OfType<GraphicsWorkspaceControl>().Single().Visibility == Visibility.Collapsed,
                "The builder or the advanced workspace is visible while the Designer is open");
            var rows = Rows();
            Require(rows.Select(r => r.Id).SequenceEqual(new[] { "title-label", "name-label", "accent", "plate" }), "Designer layer rows are not front-to-back: " + string.Join(",", rows.Select(r => r.Id)));
            Require(rows[0].Detail == "{title}" && rows[1].Detail == "{name}", "Designer text rows do not show the variable they show");
            Require(rows.Select(r => r.Label).SequenceEqual(new[] { "Title", "Name", "Accent bar", "Plate" }), "Designer rows do not show the seed's layer names: " + string.Join(",", rows.Select(r => r.Label)));
            Require(designer.ActualWidth <= width + 1, $"The Designer overflows the page horizontally ({designer.ActualWidth:0} > {width:0})");
            foreach (var name in new[] { "Add text layer", "Add shape layer", "Add image layer", "Add clock layer", "Undo", "Save design", "Save to playlist", "Typical", "Longest name", "Empty title" }) {
                var button = Button(designer, name); var point = button.TransformToVisual(designer).TransformPoint(new(0, 0));
                Require(button.ActualHeight > 0 && point.X >= -1 && point.Y >= -1 && point.X + button.ActualWidth <= designer.ActualWidth + 1 && point.Y + button.ActualHeight <= designer.ActualHeight + 1,
                    $"{name} is clipped or outside the Designer at {width:0}x{height:0} ({point.X:0},{point.Y:0} {button.ActualWidth:0}x{button.ActualHeight:0} in {designer.ActualWidth:0}x{designer.ActualHeight:0})");
            }
            if (width < 1500)
                Require(Descendants(designer).OfType<Button>().Any(b => Microsoft.UI.Xaml.Automation.AutomationProperties.GetName(b) == "Show motion" && b.ActualHeight > 0),
                    "Show motion is missing at 1360x768");
            var canvas = Descendants(designer).OfType<GraphicsAuthoringCanvas>().Single();
            Require(canvas.ActualWidth >= minCanvasWidth && canvas.ActualHeight >= minCanvasHeight, $"Designer canvas is {canvas.ActualWidth:0}x{canvas.ActualHeight:0} at {width:0}x{height:0}; needs {minCanvasWidth:0}x{minCanvasHeight:0}");
            var preview = canvas.PreviewViewport;
            var minimumPreview = width >= 1840 ? 1050 : width >= 1560 ? 960 : 740;
            Require(preview.Width >= minimumPreview && Math.Abs(preview.Width / preview.Height - 16d / 9) < .01,
                $"The fitted preview is only {preview.Width:0}x{preview.Height:0} at {width:0}x{height:0}.");
            foreach (var tabName in new[] { "Layer tab", "Variables tab", "Motion tab" })
            {
                var tab = Button(designer, tabName);
                var x = tab.TransformToVisual(designer).TransformPoint(new(0, 0)).X;
                Require(tab.FontSize <= 12 && x + tab.ActualWidth <= designer.ActualWidth,
                    $"{tabName} does not fit at compact Designer density.");
            }
            var layerScroll = Descendants(designer).OfType<ScrollViewer>().Single(v => Microsoft.UI.Xaml.Automation.AutomationProperties.GetName(v) == "Layer properties");
            var layerTop = layerScroll.TransformToVisual(designer).TransformPoint(new(0, 0)).Y;
            var visibleLayer = Math.Min(layerScroll.ViewportHeight, designer.ActualHeight - layerTop);
            if (width < 1500) {
                Require(layerScroll.Visibility == Visibility.Visible && visibleLayer >= 300, $"The Layer tab has only {visibleLayer:0} px visible at {width:0}x{height:0}");
                Invoke(Button(designer, "Variables tab")); await Task.Delay(100); designer.UpdateLayout();
                var variables = Descendants(designer).OfType<GraphicsVariablesPanel>().Single();
                var texts = Descendants(variables).OfType<TextBlock>().Select(t => t.Text).ToHashSet(StringComparer.Ordinal);
                Require(variables.ActualHeight > 0 && texts.Contains("name") && texts.Contains("title") && texts.Contains("Used by: Name"), "The Designer's Variables tab does not list name and title with Used by");
                var variablesScroll = Descendants(designer).OfType<ScrollViewer>().Single(v => Microsoft.UI.Xaml.Automation.AutomationProperties.GetName(v) == "Design variables");
                var variablesTop = variablesScroll.TransformToVisual(designer).TransformPoint(new(0, 0)).Y;
                var visibleVariables = Math.Min(variablesScroll.ViewportHeight, designer.ActualHeight - variablesTop);
                await ReviewImageWriter.SaveAsync(page, Path.Combine(directory, "graphics-designer-variables-1360x768.png"));
                Require(variablesScroll.Visibility == Visibility.Visible && visibleVariables >= 200, $"The Designer's Variables tab has only {visibleVariables:0} px visible at {width:0}x{height:0}");
                checks.Add($"At 1360x768 the Designer's Variables tab lists name and title with Used by and keeps {visibleVariables:0} px visible (>= 200)");
                Invoke(Button(designer, "Layer tab")); await Task.Delay(50); designer.UpdateLayout();
            }
            await ReviewImageWriter.SaveAsync(page, Path.Combine(directory, shot));
            checks.Add($"Designer at page {width:0}x{height:0}: one surface, layer rows front-to-back, every header/add/sample action inside, fitted preview {preview.Width:0}x{preview.Height:0}, Layer tab {visibleLayer:0} px");
        }
        // Selecting another layer restyles the rows in place (a rebuild would swallow the second tap of a double-click).
        var rowElements = Descendants(designer).OfType<Grid>().Where(g => g.Tag is GraphicsDesignerRow).ToArray();
        studio.GraphicsWorkspace.SelectLayer("plate"); await Task.Delay(50); designer.UpdateLayout();
        Require(Descendants(designer).OfType<Grid>().Where(g => g.Tag is GraphicsDesignerRow).SequenceEqual(rowElements), "Selecting a layer rebuilt the layer rows");
        checks.Add("Selecting a layer restyles the Designer's rows in place");
        Invoke(Button(designer, "Add text layer"));
        await Eventually(() => Rows().Count == 5, () => $"Add text layer did not add a row (rows={Rows().Count}, status='{studio.GraphicsWorkspace.Status}')");
        Require(studio.GraphicsWorkspace.SelectedLayerId == "text-1", $"Add text layer did not select the new layer (selected '{studio.GraphicsWorkspace.SelectedLayerId}')");
        Require(Button(designer, "Undo").IsEnabled, "Undo is disabled after an edit");
        Invoke(Button(designer, "Undo"));
        await Eventually(() => Rows().Count == 4, () => $"Undo did not remove the added layer (rows={Rows().Count})");
        checks.Add("Designer Add text layer adds and selects text-1; Undo removes it");
        await CheckLocationVariableAsync(studio, designer, Rows, checks);
        await CheckMotionAsync(page, studio, designer, checks, directory);
        await CheckContentAsync(page, studio, designer, checks, directory);
        studio.GraphicsDesignerOpen = false; await Task.Delay(80); page.UpdateLayout();
        Require(designer.Visibility == Visibility.Collapsed, "Your designs did not close the Designer");
        await CheckOperateRowVariableAsync(page, studio, seedTemplate, checks, directory);
        await CheckDataTableAsync(page, studio, checks, directory);
    }
    private static async Task CheckDataTableAsync(OverlaysPage page, StudioViewModel studio, List<string> checks, string directory)
    {
        var mode = studio.GraphicsMode;
        studio.GraphicsMode = StudioViewModel.GraphicsModeDesign;
        await Task.Delay(80);
        page.UpdateLayout();
        Require(Button(page, "Data table…").ActualHeight > 0, "Design did not show the local data import entry.");
        var data = GraphicsLocalData.Parse("id,player,score\na,Ada,9\nb,Bob,12", "csv", "id");
        var binding = new GraphicsDataBinding("probe-data", "id", ["player", "score"], "score", 4, 1);
        Require(await studio.GraphicsLibrary.SaveLocalDataAsync(binding.SourceId, "Probe scores", data) is not null,
            "The probe dataset could not be saved.");
        var row = new GraphicsPlaylistRow
        {
            Id = "probe-data", Kind = GraphicsPlaylistRow.Title, Name = "Probe leaderboard",
            TemplateId = "design-probe-data", TemplateVersion = 1, DataBinding = binding, Fields = GraphicsDataTable.Fields(data, binding)
        };
        Require(await studio.GraphicsLibrary.SavePlaylistAsync([row]), "The probe data row could not be saved.");
        studio.GraphicsMode = StudioViewModel.GraphicsModePlayout;
        await Task.Delay(100);
        var playout = Descendants(page).OfType<GraphicsPlayoutControl>().Single();
        playout.SelectRow(row.Id);
        foreach (var (width, height, label) in new[] { (1320d, 640d, "1360x768"), (1560d, 772d, "1600x900") })
        {
            page.Width = width; page.Height = height;
            await Task.Delay(80); page.UpdateLayout();
            Require(Button(playout, "Load CSV / JSON…").ActualHeight > 0 && Button(playout, "Publish data").ActualHeight > 0,
                "The data row does not expose separate import and publication actions.");
            await ReviewImageWriter.SaveAsync(page, Path.Combine(directory, $"graphics-data-table-{label}.png"));
        }
        Require(await studio.GraphicsLibrary.SavePlaylistAsync([]), "Could not clear the probe playlist.");
        studio.GraphicsMode = mode;
        checks.Add("Design exposes data-table import and Operate exposes independent load and publish actions at both viewport sizes.");
    }

    private static async Task CheckContentAsync(OverlaysPage page, StudioViewModel studio, GraphicsDesignerControl designer,
        List<string> checks, string directory)
    {
        var session = studio.GraphicsWorkspace;
        Invoke(Button(designer, "Layer tab"));
        var shapes = (MenuFlyout)Button(designer, "Add shape layer").Flyout;
        void AddShape(string label)
        {
            var item = shapes.Items.OfType<MenuFlyoutItem>().Single(item => item.Text == label);
            ((IInvokeProvider)new MenuFlyoutItemAutomationPeer(item).GetPattern(PatternInterface.Invoke)).Invoke();
        }
        AddShape("Ellipse");
        await Eventually(() => session.SelectedLayerId == "ellipse-1", () => "The Shape menu did not add and select an ellipse.");
        AddShape("Line");
        await Eventually(() => session.SelectedLayerId == "line-1", () => "The Shape menu did not add and select a line.");
        session.SelectLayer("ellipse-1");
        var stroke = Descendants(designer).OfType<Expander>().Single(item => item.Header as string == "Shape stroke");
        stroke.IsExpanded = true;
        designer.UpdateLayout();
        Descendants(stroke).OfType<NumberBox>().Single(item => item.Header as string == "Stroke width").Value = 3;
        Invoke(Button(stroke, "Apply stroke"));
        await Eventually(() => GraphicsTemplateEditor.Layer(session.TemplateDocument!, "ellipse-1").TryGetProperty("strokeWidth", out var width) &&
            width.GetDouble() == 3, () => "The ellipse stroke did not enter the private draft.");
        session.SelectLayer("plate");
        var sizing = Descendants(designer).OfType<Expander>().Single(item => item.Header as string == "Content width");
        sizing.IsExpanded = true;
        designer.UpdateLayout();
        Descendants(sizing).OfType<CheckBox>().Single(item => item.Content as string == "Size to content").IsChecked = true;
        Invoke(Button(sizing, "Apply width"));
        await Eventually(() => GraphicsTemplateEditor.Layer(session.TemplateDocument!, "plate").GetProperty("width").ValueKind ==
            System.Text.Json.JsonValueKind.String, () => "The plate did not switch to size to content.");
        session.SelectLayer("name-label");
        var effects = Descendants(designer).OfType<Expander>().Single(item => item.Header as string == "Text effects");
        effects.IsExpanded = true;
        designer.UpdateLayout();
        Descendants(effects).OfType<NumberBox>().Single(item => item.Header as string == "Outline width").Value = 2;
        Invoke(Button(effects, "Apply text effects"));
        await Eventually(() => GraphicsTemplateEditor.Layer(session.TemplateDocument!, "name-label").TryGetProperty("outline", out _),
            () => "The name did not gain an outline.");

        var svg = Path.Combine(directory, "synthetic-transparent-logo.svg");
        await File.WriteAllTextAsync(svg, """<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 10 10"><circle cx="5" cy="5" r="3" fill="#00FF00"/></svg>""");
        var previousRoot = Environment.GetEnvironmentVariable("COREVIDEO_GRAPHICS_ASSET_ROOT");
        var cache = Path.Combine(directory, "isolated-assets");
        Environment.SetEnvironmentVariable("COREVIDEO_GRAPHICS_ASSET_ROOT", cache);
        try
        {
            Require(await session.ImportDesignImageAsync(svg), "The synthetic SVG could not be staged. " + session.ImportStatus);
            Require(session.Draft?.Scratch?.PackageRoot == cache && session.SelectedLayerId == "image-1",
                "Import did not retain the asset root and select the new image layer.");
        }
        finally { Environment.SetEnvironmentVariable("COREVIDEO_GRAPHICS_ASSET_ROOT", previousRoot); }
        foreach (var (width, height, screenshot) in new[] {
            (1320d, 640d, "graphics-designer-content-1360x768.png"), (1560d, 772d, "graphics-designer-content-1600x900.png") })
        {
            page.Width = width; page.Height = height;
            await Task.Delay(80); page.UpdateLayout();
            Require(designer.ActualWidth <= width + 1 && Button(designer, "Add image layer").ActualHeight > 0,
                "Content controls overflow the Designer.");
            await ReviewImageWriter.SaveAsync(page, Path.Combine(directory, screenshot));
        }
        Require(session.CanUndo, "Content edits must be undoable.");
        while (session.CanUndo) session.Undo();
        checks.Add("Designer content adds ellipse, line and SVG image, edits stroke, outline and content width, and keeps the edits undoable.");
    }

    private static async Task CheckMotionAsync(OverlaysPage page, StudioViewModel studio, GraphicsDesignerControl designer,
        List<string> checks, string directory)
    {
        var session = studio.GraphicsWorkspace;
        page.Width = 1320; page.Height = 640;
        await Task.Delay(100); page.UpdateLayout();
        var canvas = Descendants(designer).OfType<GraphicsAuthoringCanvas>().Single();
        Require(Button(designer, "Show motion").ActualHeight > 0 && canvas.ActualWidth >= 640 && canvas.ActualHeight >= 360,
            $"Collapsed Motion strip at 1360x768 leaves a {canvas.ActualWidth:0}x{canvas.ActualHeight:0} canvas");
        Invoke(Button(designer, "Show motion"));
        await Task.Delay(100); page.UpdateLayout();
        var labels = Descendants(designer).OfType<Button>()
            .Select(Microsoft.UI.Xaml.Automation.AutomationProperties.GetName)
            .Where(name => name.Contains(" motion, starts ", StringComparison.Ordinal))
            .Select(name => name.Split(" motion, starts ")[0]).ToArray();
        Require(labels.SequenceEqual(new[] { "Plate", "Accent bar", "Name", "Title" }),
            "Motion strip order differs from paint order: " + string.Join(", ", labels));
        Require(canvas.ActualWidth >= 480 && canvas.ActualHeight >= 270 && designer.ActualWidth <= page.Width + 1,
            $"Expanded Motion strip at 1360x768 leaves a {canvas.ActualWidth:0}x{canvas.ActualHeight:0} canvas");
        session.SelectLayer("plate");
        Invoke(Button(designer, "Motion tab"));
        await Task.Delay(50); designer.UpdateLayout();
        Invoke(Button(designer, "Wipe"));
        await Eventually(() => session.TemplateDocument is { } document &&
            GraphicsTemplateEditor.LayerMotion(document, "plate", "in")?.Type == "wipe",
            () => "Selecting Wipe did not give the plate its own motion.");
        designer.UpdateLayout();
        Require(Button(designer, "Plate motion, starts 0.00 s, takes 0.35 s").ActualHeight > 0, "The strip did not show the plate wipe.");
        session.SelectLayer("name-label");
        Invoke(Button(designer, "Fade + rise"));
        await Task.Delay(50); designer.UpdateLayout();
        var start = Descendants(designer).OfType<TextBox>().Single(box =>
            Microsoft.UI.Xaml.Automation.AutomationProperties.GetName(box) == "Starts at");
        start.Text = "0.20 s";
        Invoke(Button(designer, "Play preview"));
        await Eventually(() => session.PreviewPlay?.Mode == "cycle", () => "Play preview did not start a private cycle.");
        Require(session.TemplateDocument is { } edited && GraphicsTemplateEditor.LayerMotion(edited, "name-label", "in")?.StartMs == 200,
            "The name should begin 0.20 s after the plate.");
        checks.Add($"Motion at 1360x768 starts collapsed; expanded canvas {canvas.ActualWidth:0}x{canvas.ActualHeight:0}. Plate wipes and Name rises at 0.20 s.");
        await ReviewImageWriter.SaveAsync(page, Path.Combine(directory, "graphics-designer-motion-1360x768.png"));

        page.Width = 1560; page.Height = 772;
        await Task.Delay(100); page.UpdateLayout();
        Require(canvas.ActualWidth >= 640 && canvas.ActualHeight >= 360, $"Expanded Motion strip at 1600x900 leaves a {canvas.ActualWidth:0}x{canvas.ActualHeight:0} canvas");
        foreach (var name in new[] { "Play preview", "Play In", "Play Out", "Loop preview", "Out mirrors In", "Motion tab" })
        {
            var element = Descendants(designer).OfType<FrameworkElement>().Single(item =>
                Microsoft.UI.Xaml.Automation.AutomationProperties.GetName(item) == name);
            var point = element.TransformToVisual(designer).TransformPoint(new(0, 0));
            Require(element.ActualHeight > 0 && point.X >= -1 && point.Y >= -1 &&
                point.X + element.ActualWidth <= designer.ActualWidth + 1 && point.Y + element.ActualHeight <= designer.ActualHeight + 1,
                $"{name} is clipped at 1600x900");
        }
        Invoke(Button(designer, "Bar first, words after"));
        await Eventually(() => session.TemplateDocument is { } preset &&
            GraphicsTemplateEditor.LayerMotion(preset, "accent", "in")?.Type == "wipe", () => "The bar-first preset did not apply.");
        designer.UpdateLayout();
        Require(Button(designer, "Name motion, starts 0.20 s, takes 0.30 s").ActualHeight > 0, "The bar-first preset did not show Name at 0.20 s.");
        await ReviewImageWriter.SaveAsync(page, Path.Combine(directory, "graphics-designer-motion-1600x900.png"));
        Invoke(Button(designer, "Play preview"));
        await Eventually(() => session.PreviewPlay?.Mode == "cycle", () => "The second Play preview did not start a private cycle.");
        Invoke(Button(designer, "Undo"));
        await Eventually(() => session.PreviewPlay is null && session.TemplateDocument is { } restored &&
            GraphicsTemplateEditor.LayerMotion(restored, "accent", "in") is null, () => "Undo did not restore the draft and stop the private play.");
        for (var i = 0; i < 3; i++) session.Undo();
        checks.Add($"Motion at 1600x900 keeps all controls inside the Designer and a {canvas.ActualWidth:0}x{canvas.ActualHeight:0} canvas; preset, Play preview and Undo passed.");
    }

    /// <summary>Acceptance 1 in the Designer (final review C1b / M5): add <c>location</c> (Text, 40), give it a Sample value
    /// (the draft follows), add a text layer and choose Shows: Variable location in the ComboBox (the inspector rebuild is
    /// deferred past the box's own SelectionChanged). Undo then restores the probe draft.</summary>
    private static async Task CheckLocationVariableAsync(StudioViewModel studio, GraphicsDesignerControl designer, Func<IReadOnlyList<GraphicsDesignerRow>> rows, List<string> checks)
    {
        var session = studio.GraphicsWorkspace;
        Invoke(Button(designer, "Variables tab")); await Task.Delay(80); designer.UpdateLayout();
        var panel = Descendants(designer).OfType<GraphicsVariablesPanel>().Single();
        Descendants(panel).OfType<TextBox>().Single(t => Microsoft.UI.Xaml.Automation.AutomationProperties.GetName(t) == "New variable name").Text = "location";
        Descendants(panel).OfType<NumberBox>().Single(n => Microsoft.UI.Xaml.Automation.AutomationProperties.GetName(n) == "New variable max length").Value = 40;
        Invoke(Button(panel, "Add variable"));
        await Eventually(() => session.Draft?.Fields.TryGetProperty("location", out _) == true, () => $"Add variable did not add location (status='{session.Status}')");
        Require(session.TemplateDocument is { } added && GraphicsTemplateEditor.TemplateFields(added).Single(f => f.Id == "location").MaxLength == 40, "location was not added as Text (40)");
        await Task.Delay(80); panel.UpdateLayout();
        Invoke(Button(panel, "Add Sources column"));
        await Eventually(() => studio.GraphicsLibrary.Document.SourceColumns.Any(column => column.Id == "location"),
            "The Designer did not add Location to Sources.");
        checks.Add("Designer Variables offers a Sources column for an extra variable and saves its stable id.");
        Descendants(panel).OfType<TextBox>().Single(t => t.Header as string == "Sample value").Text = "Oslo";
        Invoke(Button(panel, "Apply variable options"));
        await Eventually(() => session.Draft?.Fields.GetProperty("location").GetString() == "Oslo", () => $"Sample value did not seed the draft's location (draft='{session.Draft?.Fields.GetProperty("location")}')");
        checks.Add("Designer Variables: add location (Text, 40); its Sample value Oslo also becomes the draft's value (Typical shows it)");
        Invoke(Button(designer, "Layer tab")); await Task.Delay(50); designer.UpdateLayout();
        Invoke(Button(designer, "Add text layer"));
        await Eventually(() => session.SelectedLayerId == "text-1" && Descendants(designer).OfType<ComboBox>().Any(c => c.Header as string == "Shows"), () => $"Add text layer did not select text-1 with a Shows box (selected '{session.SelectedLayerId}')");
        var shows = Descendants(designer).OfType<ComboBox>().Single(c => c.Header as string == "Shows");
        shows.SelectedValue = "location";
        await Eventually(() => rows().Any(r => r.Id == "text-1" && r.Detail == "{location}"), () => $"Shows: Variable location did not bind text-1 (rows={string.Join(",", rows().Select(r => r.Id + "=" + r.Detail))}, status='{session.Status}')");
        await Eventually(() => Descendants(designer).OfType<ComboBox>().SingleOrDefault(c => c.Header as string == "Shows") is { SelectedValue: "location" } rebuilt && !ReferenceEquals(rebuilt, shows),
            "The inspector was not rebuilt with Shows: Variable location after the deferred edit");
        Require(session.SelectedLayerId == "text-1", "Choosing Shows moved the layer selection");
        checks.Add("Designer: Add text layer then Shows: Variable location binds the new layer; the inspector rebuilds after the ComboBox's own event");
        for (var i = 0; i < 8 && session.CanUndo; i++) { Invoke(Button(designer, "Undo")); await Task.Delay(30); }
        await Eventually(() => rows().Count == 4 && session.Draft?.Fields.TryGetProperty("location", out _) == false, () => $"Undo did not restore the probe draft (rows={rows().Count})");
    }
    /// <summary>Final review C1a: a Guest design row (following Sources) whose template has an extra text variable shows a
    /// box for it in Operate's row inspector (header = the variable's label, max length = its limit).</summary>
    private static async Task CheckOperateRowVariableAsync(OverlaysPage page, StudioViewModel studio, NativeGraphicsTemplate seedTemplate, List<string> checks, string directory)
    {
        var withLocation = GraphicsTemplateEditor.AddField(NativeGraphicsScratch.From(seedTemplate.Definition!.Value), "location", "text", 40);
        var manifest = System.Text.Json.Nodes.JsonNode.Parse(withLocation.Manifest)!.AsObject(); manifest["templateVersion"] = 2;
        var definition = System.Text.Json.JsonSerializer.SerializeToElement(manifest);
        var v2 = System.Text.Json.JsonSerializer.Deserialize<NativeGraphicsTemplate>(manifest.ToJsonString(), new System.Text.Json.JsonSerializerOptions(System.Text.Json.JsonSerializerDefaults.Web))! with {
            Runtime = "declarative", Definition = definition, ManifestHash = NativeGraphicsScratch.From(definition).Hash };
        var before = studio.GraphicsWorkspace.State;
        studio.GraphicsWorkspace.Observe((before ?? new()) with { Templates = [.. before?.Templates ?? [], v2] });
        static System.Text.Json.JsonElement Text(string value) => System.Text.Json.JsonSerializer.SerializeToElement(value);
        var row = new GraphicsPlaylistRow { Id = "probe-strap", Kind = GraphicsPlaylistRow.Guest, Name = "Probe strap", FollowSources = true, Hotkey = "F6", PlayOutAfterMs = 8000,
            TemplateId = v2.TemplateId, TemplateVersion = 2, Fields = new Dictionary<string, System.Text.Json.JsonElement> { ["name"] = Text("Name"), ["title"] = Text("Title"), ["location"] = Text("Oslo") } };
        Require(await studio.GraphicsLibrary.SavePlaylistAsync([row]), "The probe design row could not be saved: " + studio.GraphicsLibrary.Status);
        var mode = studio.GraphicsMode;
        studio.GraphicsMode = StudioViewModel.GraphicsModePlayout; await Task.Delay(100); page.UpdateLayout();
        var playout = Descendants(page).OfType<GraphicsPlayoutControl>().Single();
        Require(playout.Visibility == Visibility.Visible, "Operate did not show the playlist");
        playout.SelectRow("probe-strap"); await Task.Delay(100); page.UpdateLayout();
        TextBox? Location() => Descendants(playout).OfType<TextBox>().SingleOrDefault(t => Microsoft.UI.Xaml.Automation.AutomationProperties.GetName(t) == "Row variable location");
        await Eventually(() => Location() is { ActualHeight: > 0 }, "Operate's row inspector has no box for the design's location variable");
        var box = Location()!;
        Require(box.Header as string == "Location" && box.MaxLength == 40 && box.Text == "Oslo", $"The location box is wrong (header='{box.Header}', max={box.MaxLength}, text='{box.Text}')");
        Require(Descendants(playout).OfType<TextBox>().Any(t => t.Header as string == "Title line" && t.IsReadOnly), "The Guest row's Title line is not the read-only Sources text");
        await ReviewImageWriter.SaveAsync(page, Path.Combine(directory, "graphics-operate-row-variable.png"));
        checks.Add("Operate: a Guest design row with an extra text variable shows a Location box (max 40, row value) beside the Sources title line");
        // Re-review m1: a saved value updates the box in place; the inspector is not rebuilt (a rebuild would drop focus).
        var saved = studio.GraphicsLibrary.Document.Playlist.Single();
        Require(await studio.GraphicsLibrary.SavePlaylistAsync([GraphicsRowFields.WithText(saved, "location", "Bergen")]), "The probe row could not be re-saved: " + studio.GraphicsLibrary.Status);
        await Eventually(() => box.Text == "Bergen", () => $"The Location box did not show the saved value (text='{box.Text}')");
        Require(ReferenceEquals(Location(), box), "Saving a row value rebuilt the inspector instead of updating the box in place");
        checks.Add("Operate: a saved row value updates the Location box in place, without rebuilding the inspector");
        Require(await studio.GraphicsLibrary.AddSourceColumnAsync("office"), "The probe could not add Office.");
        await Eventually(() => Descendants(playout).OfType<ComboBox>().Any(c => c.Header as string == "Location source"),
            "Operate did not offer a Sources column for Location.");
        Require(Descendants(playout).OfType<DropDownButton>().Any(button => button.Content as string == "Names list…"),
            "A design row could not open the names-list editor from Operate.");
        var source = Descendants(playout).OfType<ComboBox>().Single(c => c.Header as string == "Location source");
        source.SelectedItem = "Office";
        await Eventually(() => studio.GraphicsLibrary.Document.Playlist.Single().SourceMappings.GetValueOrDefault("location") == "office",
            "Operate did not save the Location mapping.");
        foreach (var (width, height, label) in new[] { (1320d, 640d, "1360x768"), (1560d, 772d, "1600x900") })
        {
            page.Width = width; page.Height = height;
            await Task.Delay(80); page.UpdateLayout();
            Require(Descendants(playout).OfType<ComboBox>().Any(c => c.Header as string == "Location source" && c.ActualHeight > 0),
                "The Location mapping control did not lay out.");
            await ReviewImageWriter.SaveAsync(page, Path.Combine(directory, $"graphics-operate-source-mapping-{label}.png"));
        }
        checks.Add("Operate saves the selected Sources column for the pinned Location variable.");
        Require(await studio.GraphicsLibrary.SavePlaylistAsync([]), "The probe playlist could not be cleared");
        if (before is not null) studio.GraphicsWorkspace.Observe(before);
        studio.GraphicsMode = mode; await Task.Delay(80); page.UpdateLayout();
    }
    private static async Task CheckAutomationEditingAsync(Window window, StudioViewModel studio, List<string> checks)
    {
        var rule = new GraphicsAutomationRule { Id="rule-a", InstanceId="intro", SourceId="guest-a" };
        var state = new NativeGraphicsState { CoreEpoch="automation-ui", Instances=[new() {InstanceId="intro",TemplateId="compact-solid",Label="Introduction"}],
            Automation=new() {Mode="automatic",Configuration=new() {Rules=[rule]}},
            Sources=[new() {DisplayName="Alice",VideoOn=true,Identity=new() {SourceId="guest-a",InstanceId="camera-a",ProcessEpoch="automation-ui",Generation=1}}] };
        studio.GraphicsWorkspace.Observe(state);
        var control = new GraphicsAutomationControl(studio);
        window.Content = new ScrollViewer {Content=control};
        await Eventually(()=>control.IsLoaded);
        foreach(var expander in Descendants(control).OfType<Expander>().ToArray())expander.IsExpanded=true;
        await Eventually(()=>Descendants(control).OfType<SliderValueControl>().Any(s=>s.Header=="Subject dwell"));
        var mode=Descendants(control).OfType<ComboBox>().Single(c=>c.Header as string=="Mode");
        var subject=Descendants(control).OfType<ComboBox>().Single(c=>c.Header as string=="Subject");
        var dwell=Descendants(control).OfType<SliderValueControl>().Single(s=>s.Header=="Subject dwell");
        Require(mode.SelectedItem as string=="Automatic", "Mode selector did not reflect observed native Automatic mode");
        state=state with {Automation=state.Automation! with {Mode="rehearsal"}};
        studio.GraphicsWorkspace.Observe(state);control.Refresh();
        Require(mode.SelectedItem as string=="Rehearsal", "External native mode change did not refresh clean selector");
        mode.SelectedIndex=0;
        state=state with {Automation=state.Automation! with {Armed=true}};
        studio.GraphicsWorkspace.Observe(state);control.Refresh();
        Require(mode.SelectedItem as string=="Manual", "Runtime refresh erased unsent mode selection");
        Invoke(Button(control,"Discard edits and reload current settings"));
        await Eventually(()=>mode.SelectedItem as string=="Rehearsal");
        dwell.Value=3.5;
        state=state with {Sources=[]};
        studio.GraphicsWorkspace.Observe(state);control.Refresh();
        Require(dwell.Value==3.5 && subject.SelectedItem?.ToString()=="Unavailable source · guest-a", "Roster refresh erased pending dwell or silently substituted an automatic subject");
        Require(!Button(control,"Arm").IsEnabled && !Button(control,"Save rule configuration").IsEnabled,"Pending form edits can be mistaken for saved or armed rules");
        state=state with {Automation=state.Automation! with {Revision=2,Configuration=new() {Rules=[rule with {DwellMs=5000}]}}};
        studio.GraphicsWorkspace.Observe(state);control.Refresh();
        Require(dwell.Value==3.5 && !Button(control,"Apply rule — disarmed").IsEnabled,"Concurrent rule edit was overwritten or pending conflicting edits can be applied");
        Invoke(Button(control,"Discard edits and reload current settings"));
        await Eventually(()=>dwell.Value==5);
        Require(subject.SelectedItem?.ToString()=="Unavailable source · guest-a", "Reload lost the explicit unavailable guest binding");
        checks.Add("compiled automation editor follows native mode, retains unsent mode/rule edits and missing guest identity, blocks conflicts/arming/save, and explicitly reloads newer settings without sending media commands");
    }
    private static async Task CheckBusyTextBindingAsync(Window window, List<string> checks)
    {
        var text = new TextBox { Text="Before", Header="Isolated pending edit" };
        window.Content = new StackPanel { Children = { text } };
        await Eventually(()=>text.IsLoaded);
        var stored = "Before"; var editable = false; var writes = 0;
        using var binding = new GraphicsTextFieldBinding(text, ()=>stored, value=> {
            if (!editable) return false;
            stored=value; writes++; return true;
        }, ()=>{}, ()=>{});
        text.Text="Pending user edit";
        binding.Refresh(); await Task.Delay(50);
        Require(stored=="Before" && text.Text=="Pending user edit","A busy model discarded pending text");
        editable=true; binding.Refresh();
        await Eventually(()=>stored=="Pending user edit","Pending text did not commit after the model became editable");
        binding.Refresh(); await Task.Delay(50);
        Require(writes==1,"Model refresh echoed text back as another edit");
        binding.Dispose(); text.Text="Retired callback"; await Task.Delay(50);
        Require(stored=="Pending user edit","Disposed text binding wrote into the model");
        checks.Add("busy sessions retain pending input, retry once editable, avoid model echo and reject disposed callbacks");
    }
    private static Button Button(DependencyObject root,string name)=>Descendants(root).OfType<Button>().Single(b=>b.Content?.ToString()==name || Microsoft.UI.Xaml.Automation.AutomationProperties.GetName(b)==name);
    private static void Invoke(Button button)=>((IInvokeProvider)new ButtonAutomationPeer(button).GetPattern(PatternInterface.Invoke)).Invoke();
    private static IEnumerable<DependencyObject> Descendants(DependencyObject root) { for(var i=0;i<VisualTreeHelper.GetChildrenCount(root);i++) {var child=VisualTreeHelper.GetChild(root,i);yield return child;foreach(var descendant in Descendants(child))yield return descendant;} }
    private static void Require(bool value,string reason) {if(!value)throw new InvalidOperationException(reason);}
    // Reason is evaluated after the wait, so a timeout reports the state it timed out in (#970).
    private static async Task Eventually(Func<bool> predicate, Func<string> reason) {var watch=Stopwatch.StartNew();while(!predicate()&&watch.Elapsed<TimeSpan.FromSeconds(10))await Task.Delay(30);Require(predicate(),predicate()?"":reason());}
    private static async Task Eventually(Func<bool> predicate, string reason="Compiled Graphics controls did not load") {var watch=Stopwatch.StartNew();while(!predicate()&&watch.Elapsed<TimeSpan.FromSeconds(10))await Task.Delay(30);Require(predicate(),reason);}
}
