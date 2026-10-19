using System.Diagnostics;
using System.Text.Json;
using CoreVideoPro.MediaCore.Services;
using CoreVideoPro.WinUI.ViewModels;
using CoreVideoPro.WinUI.Views;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls.Primitives;
using Microsoft.UI.Xaml.Media;
using Windows.Graphics;

namespace CoreVideoPro.WinUI.Services;

// Full compiled Studio geometry, not a cropped Graphics page. Owns an isolated
// offscreen window and never starts a core, outputs, meeting or desktop input.
internal static class StudioWorkspaceLayoutProbe
{
    internal static async Task CheckAsync(Window window, List<string> checks, string directory)
    {
        var previous = Environment.GetEnvironmentVariable("COREVIDEO_GRAPHICS_DOCUMENT_PATH");
        Environment.SetEnvironmentVariable("COREVIDEO_GRAPHICS_DOCUMENT_PATH", Path.Combine(directory, "isolated-" + Guid.NewGuid().ToString("N") + ".json"));
        try
        {
            await using var studio = new StudioViewModel(StudioStartupBootstrap.Create(new InMemoryProductionOutputPreferencesStore(), startInteractiveServices: false));
            var page = new StudioWorkspace { ViewModel = studio, RequestedTheme = ElementTheme.Dark };
            window.Content = page;
            window.AppWindow.MoveAndResize(new RectInt32(-20000, -20000, 1920, 1080));
            window.AppWindow.Show(false);
            var watch = Stopwatch.StartNew();
            while (!page.IsLoaded && watch.Elapsed < TimeSpan.FromSeconds(10)) await Task.Delay(30);
            if (!page.IsLoaded) throw new InvalidOperationException("Studio did not load");
            if (Math.Abs(page.XamlRoot.RasterizationScale-1)>0.001) throw new InvalidOperationException("Studio fit qualification requires 100% scaling; observed " + page.XamlRoot.RasterizationScale);
            var failures = new List<string>();
            var metrics = new List<object>();
            foreach (var (width, height) in new[] { (1904, 1000), (1600, 900), (1424, 860), (1360, 768) })
            {
                page.Width = width; page.Height = height;
                page.HorizontalAlignment = HorizontalAlignment.Left; page.VerticalAlignment = VerticalAlignment.Top;
                await Task.Delay(150); page.UpdateLayout();
                var footer = (FrameworkElement)page.FindName("ProductionFooter");
                var automation = (FrameworkElement)page.FindName("FooterAutomation");
                var transport = (FrameworkElement)page.FindName("FooterTransport");
                var a = automation.TransformToVisual(page).TransformPoint(new(0, 0));
                var t = transport.TransformToVisual(page).TransformPoint(new(0, 0));
                metrics.Add(new {width,height,group="footer",automationWidth=automation.DesiredSize.Width,transportWidth=transport.DesiredSize.Width});
                if (Math.Min(a.X + automation.ActualWidth, t.X + transport.ActualWidth) - Math.Max(a.X, t.X) > 1 &&
                    Math.Min(a.Y + automation.ActualHeight, t.Y + transport.ActualHeight) - Math.Max(a.Y, t.Y) > 1)
                    failures.Add($"{width}x{height}: automation and transport overlap");
                ((Microsoft.UI.Xaml.Controls.TextBlock)page.FindName("AssignedLookLabel")).Text = "Panel discussion · Quarterly customer leadership summit guest introduction";
                foreach(var name in new[] {"ProgramOutputHealth","StreamOutputHealth","RecordOutputHealth","AutomationStatusText"})
                    ((Microsoft.UI.Xaml.Controls.TextBlock)page.FindName(name)).Text="Waiting for destination connection and source readiness";
                page.UpdateLayout();
                var after=transport.TransformToVisual(page).TransformPoint(new(0,0));
                if(Math.Abs(after.X-t.X)>1 || Math.Abs(after.Y-t.Y)>1)failures.Add($"{width}x{height}: live status text moved the transport controls");
                if(((Microsoft.UI.Xaml.Controls.ScrollViewer)page.FindName("FooterTelemetry")).ScrollableWidth>1)failures.Add($"{width}x{height}: output health or audio controls need horizontal scrolling");
                var controls = Descendants(footer).OfType<ButtonBase>().Where(Visible).Where(e => e.ActualWidth > 0 && e.ActualHeight > 0).ToArray();
                foreach (var control in controls)
                {
                    var point = control.TransformToVisual(page).TransformPoint(new(0, 0));
                    var name = Microsoft.UI.Xaml.Automation.AutomationProperties.GetName(control);
                    if (name.Length == 0) name = control.Content?.ToString() ?? control.GetType().Name;
                    metrics.Add(new { width, height, name, x = point.X, y = point.Y, w = control.ActualWidth, h = control.ActualHeight });
                    if (point.X < -1 || point.Y < -1 || point.X + control.ActualWidth > width + 1 || point.Y + control.ActualHeight > height + 1)
                        failures.Add($"{width}x{height}: {name} falls outside Studio ({point.X:F0},{point.Y:F0},{control.ActualWidth:F0},{control.ActualHeight:F0})");
                }
                await ReviewImageWriter.SaveAsync(page, Path.Combine(directory, $"studio-{width}x{height}.png"));
                checks.Add($"Full Studio: {controls.Length} footer actions contained, automation/transport disjoint, long assigned label bounded at {width}x{height}; raster scale {page.XamlRoot.RasterizationScale}; native video pixels excluded");
            }
            foreach(var (width,height) in new[] {(1904,1000),(1424,860),(1360,768)})
            {
                page.Width=width;page.Height=height;
                studio.ActiveTab=CoreVideoPro.WinUI.Models.StudioTab.Overlays;
                await Task.Delay(150);page.UpdateLayout();
                if(((FrameworkElement)page.FindName("ProductionFooter")).Visibility!=Visibility.Collapsed) failures.Add("Graphics still exposes the live production footer");
                var cards=Descendants(page).OfType<Microsoft.UI.Xaml.Controls.Button>().Where(Visible).Where(b=>GraphicsShowStarters.All.Any(s=>s.Name==Microsoft.UI.Xaml.Automation.AutomationProperties.GetName(b))).ToArray();
                if(cards.Length!=4)failures.Add($"{width}x{height}: Graphics does not expose four show looks");
                foreach(var button in cards)
                {
                    var point=button.TransformToVisual(page).TransformPoint(new(0,0));
                    if(point.X<0 || point.Y<0 || point.X+button.ActualWidth>width+1 || point.Y+button.ActualHeight>height+1)failures.Add($"{width}x{height}: full-workspace Graphics card clipped");
                }
                await ReviewImageWriter.SaveAsync(page,Path.Combine(directory,$"graphics-full-{width}x{height}.png"));
                studio.ActiveTab=CoreVideoPro.WinUI.Models.StudioTab.Studio;page.UpdateLayout();
                if(((FrameworkElement)page.FindName("ProductionFooter")).Visibility!=Visibility.Visible) failures.Add("Returning to Studio did not restore production controls");
                checks.Add($"Graphics private workspace fits inside the complete {width}x{height} shell; returning to Studio restores its production controls");
            }
            await File.WriteAllTextAsync(Path.Combine(directory, "geometry.json"), JsonSerializer.Serialize(new { metrics, failures }, new JsonSerializerOptions { WriteIndented = true }));
            if (failures.Count > 0) throw new InvalidOperationException(string.Join("; ", failures));
        }
        finally { Environment.SetEnvironmentVariable("COREVIDEO_GRAPHICS_DOCUMENT_PATH", previous); }
    }
    private static bool Visible(FrameworkElement e)
    {
        for (DependencyObject? p = e; p is not null; p = VisualTreeHelper.GetParent(p))
            if (p is FrameworkElement { Visibility: Visibility.Collapsed }) return false;
        return true;
    }
    private static IEnumerable<DependencyObject> Descendants(DependencyObject root)
    {
        for (var i = 0; i < VisualTreeHelper.GetChildrenCount(root); i++)
        {
            var child = VisualTreeHelper.GetChild(root, i); yield return child;
            foreach (var descendant in Descendants(child)) yield return descendant;
        }
    }
}
