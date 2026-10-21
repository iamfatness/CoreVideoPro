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
            // Reserve the existing navigation/header/production footer rather
            // than pretending Graphics can consume the entire display.
            page.Width = 1840; page.Height = 820; page.HorizontalAlignment = HorizontalAlignment.Left; page.VerticalAlignment = VerticalAlignment.Top;
            var template = new NativeGraphicsTemplate { TemplateId="compact-solid", TemplateVersion=1, Name="Compact solid", Family="lower-third", Runtime="compatibility" };
            var catalog = new[] { template, template with { TemplateId="minimal-accent",Name="Minimal accent" }, template with { TemplateId="broadcast",Name="Broadcast" },
                new NativeGraphicsTemplate {TemplateId="centered-title",TemplateVersion=1,Name="Centered title",Family="title",Runtime="declarative",Fields=[new() {Id="heading",Type="string",Default=System.Text.Json.JsonSerializer.SerializeToElement("Welcome")},new() {Id="subtitle",Type="string",Default=System.Text.Json.JsonSerializer.SerializeToElement("Your show")}]},
                new NativeGraphicsTemplate {TemplateId="corner-logo",TemplateVersion=1,Name="Logo",Family="bug",Runtime="declarative"},
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
            await ReviewImageWriter.SaveAsync(page,Path.Combine(directory,"graphics-try-interview-1080.png"));
            checks.Add("Four visible show-template starters; Interview opens populated private samples; branding and per-graphic edits survive navigation without creating native instances or assigning Studio");
            Invoke(Button(workspace,"Show templates"));Invoke(Button(workspace,"Browse individual graphics"));await Task.Delay(80);
            var individualLibrary=Descendants(workspace).OfType<ListView>().Single(l=>l.IsItemClickEnabled);
            individualLibrary.SelectedIndex=0;Invoke(Button(workspace,"Close"));await Task.Delay(80);
            Require(sampleList.Visibility==Visibility.Collapsed && studio.GraphicsWorkspace.Selected is null && studio.GraphicsWorkspace.Draft?.TemplateId==catalog[0].TemplateId,"Individual library browsing retained the show-starter mode or hid the editor");
            checks.Add("Returning to individual graphics from a starter exits sample-set mode and preserves the original library workflow");
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
            search.Text="Show cue 2"; await Task.Delay(50);
            Require(cueList.Items.Count==1 && studio.GraphicsWorkspace.SelectedId=="fixture-1","Cue filtering changed selection or returned the wrong cue");
            search.Text=""; var scope=Descendants(workspace).OfType<ComboBox>().Single(c=>c.Header?.ToString()=="Scope"); scope.SelectedIndex=1; await Task.Delay(50);
            Require(cueList.Items.Count==7,"Current Program scope did not retain global cues and hide the other scene"); scope.SelectedIndex=0;
            Require(cueList.Items.Count==8 && studio.GraphicsWorkspace.SelectedId=="fixture-1","Clearing cue filters lost the selected cue");
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
        } finally { Environment.SetEnvironmentVariable("COREVIDEO_GRAPHICS_DOCUMENT_PATH",previousPath); }
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
    private static async Task Eventually(Func<bool> predicate, string reason="Compiled Graphics controls did not load") {var watch=Stopwatch.StartNew();while(!predicate()&&watch.Elapsed<TimeSpan.FromSeconds(10))await Task.Delay(30);Require(predicate(),reason);}
}
