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
            var state = new NativeGraphicsState { CoreEpoch="isolated-layout", SchemaVersion=1, Templates=[template],
                Instances=Enumerable.Range(1,8).Select(n=>new NativeGraphicsInstance { InstanceId=$"fixture-{n}", Label=n==1?"Guest introduction":$"Show cue {n}", TemplateId=template.TemplateId, TemplateVersion=1, Scope="global", Revision=1, Fields=new() { Name="Alex Rivera",Title="Producer · Office Hours" } }).ToArray() };
            studio.GraphicsWorkspace.Observe(state); studio.GraphicsWorkspace.Select("fixture-1");
            await Task.Delay(250); page.UpdateLayout();
            var workspace = Descendants(page).OfType<GraphicsWorkspaceControl>().Single();
            Require(workspace.ActualHeight <= 821 && workspace.ActualWidth <= 1841, "Graphics exceeded the 1080p content budget");
            var canvas = Descendants(workspace).OfType<GraphicsAuthoringCanvas>().Single();
            await ReviewImageWriter.SaveAsync(page,Path.Combine(directory,"graphics-942-initial-1080.png"));
            Require(canvas.ActualWidth > 700 && canvas.ActualHeight > 350, $"Native canvas was crowded out: {canvas.ActualWidth} x {canvas.ActualHeight}; workspace {workspace.ActualWidth} x {workspace.ActualHeight}");
            foreach (var title in new[] {"Cue to Preview","Show on Program","Hide","Update on air","Cancel draft","Clear graphics","Use in Studio"}) {
                var button=Button(workspace,title); var point=button.TransformToVisual(workspace).TransformPoint(new(0,0));
                Require(button.ActualHeight>0 && point.Y>=0 && point.Y+button.ActualHeight<=workspace.ActualHeight+1 && point.X+button.ActualWidth<=workspace.ActualWidth+1, $"{title} is clipped or below the workspace");
            }
            checks.Add("1840 x 820 shell content budget on 1920 x 1080: native canvas, inspector and all fixed actions stay visible; eight cue rows bounded");
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
            Invoke(Button(workspace,"Use in Studio"));
            await Eventually(()=>studio.GraphicsLibrary.Document.StudioLowerThird is not null && !studio.GraphicsLibrary.Busy);
            Require(studio.StudioAssignedLowerThirdLabel=="Guest introduction","Assigned look label did not update after explicit assignment");
            studio.GraphicsWorkspace.Select("fixture-2");
            Require(studio.StudioAssignedLowerThirdLabel=="Guest introduction","Browsing changed the assigned look label");
            checks.Add("Use in Studio saves the selected look and updates the bound Studio label; later cue selection preserves assignment");
        } finally { Environment.SetEnvironmentVariable("COREVIDEO_GRAPHICS_DOCUMENT_PATH",previousPath); }
    }
    private static Button Button(DependencyObject root,string name)=>Descendants(root).OfType<Button>().Single(b=>b.Content?.ToString()==name);
    private static void Invoke(Button button)=>((IInvokeProvider)new ButtonAutomationPeer(button).GetPattern(PatternInterface.Invoke)).Invoke();
    private static IEnumerable<DependencyObject> Descendants(DependencyObject root) { for(var i=0;i<VisualTreeHelper.GetChildrenCount(root);i++) {var child=VisualTreeHelper.GetChild(root,i);yield return child;foreach(var descendant in Descendants(child))yield return descendant;} }
    private static void Require(bool value,string reason) {if(!value)throw new InvalidOperationException(reason);}
    private static async Task Eventually(Func<bool> predicate) {var watch=Stopwatch.StartNew();while(!predicate()&&watch.Elapsed<TimeSpan.FromSeconds(10))await Task.Delay(30);Require(predicate(),"Compiled Graphics controls did not load");}
}
