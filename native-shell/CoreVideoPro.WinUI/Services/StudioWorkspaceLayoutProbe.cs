using System.Diagnostics;
using System.Text.Json;
using CoreVideoPro.MediaCore.Contracts;
using CoreVideoPro.MediaCore.Models;
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
                var actionRow = (FrameworkElement)page.FindName("FooterOutputActions");
                var actionTop = actionRow.TransformToVisual(page).TransformPoint(new(0, 0)).Y;
                if (Math.Abs(actionRow.ActualHeight - 48) > 1) failures.Add($"{width}x{height}: output actions are not a single 48px row");
                if (Math.Abs(a.Y - t.Y) < 1 && Math.Abs(a.Y - actionTop) > 1) failures.Add($"{width}x{height}: adjacent automation and output action baselines differ");
                var telemetry = (FrameworkElement)page.FindName("FooterTelemetry");
                var headings = Descendants(telemetry).OfType<Microsoft.UI.Xaml.Controls.TextBlock>()
                    .Where(text => text.Text is "PROGRAM" or "STREAM" or "RECORD" or "FRAME DROPS" or "ELAPSED").ToArray();
                var headingTops = headings.Select(text => text.TransformToVisual(page).TransformPoint(new(0, 0)).Y).ToArray();
                if (headings.Length != 5 || headingTops.Max() - headingTops.Min() > 1) failures.Add($"{width}x{height}: telemetry headings have inconsistent baselines");
                if (Descendants(footer).OfType<Microsoft.UI.Xaml.Controls.TextBlock>().Any(text => text.Text == "Outputs idle")) failures.Add("Redundant persistent output summary returned");
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
            page.Width = 1424; page.Height = 860;
            foreach (var state in new[] { "preparing", "producing", "interrupted", "failed", "finalizing", "completed", "idle" })
            {
                var lifecycle = new OutputLifecycle { SessionId = "isolated-status", DesiredActive = true, State = state, Health = state is "failed" or "interrupted" ? "failed" : "healthy", Finalized = state == "completed" };
                var snapshot = new NativeMediaCoreStateSnapshot
                {
                    Recording = new NativeMediaCoreRecordingSession { SessionId = "isolated-status", Active = true, Status = "recording", WriterStatus = "writing", Lifecycle = lifecycle, TargetFolder = "isolated", FilenamePrefix = "test", Format = "mp4", Quality = "high", ProgramPath = "isolated/Program.mp4", TotalFramesWritten = 120 },
                    OutputSenderSession = new NativeMediaCoreOutputSenderSession { Status = "live", Senders = [new NativeMediaCoreOutputSender { SenderId = "isolated-status", Destination = "rtmp", Status = "live", Lifecycle = lifecycle, FramesSent = 180 }] }
                };
                studio.Transport.ApplySnapshot(snapshot, true, true, "1080p60", true);
                await Task.Delay(30); page.UpdateLayout();
                var record = StudioOutputTelemetry.Recording(snapshot.Recording);
                var stream = StudioOutputTelemetry.Streaming(snapshot);
                if (((Microsoft.UI.Xaml.Controls.TextBlock)page.FindName("RecordOutputHealth")).Text != record.State ||
                    ((Microsoft.UI.Xaml.Controls.TextBlock)page.FindName("StreamOutputHealth")).Text != stream.State)
                    failures.Add($"{state}: observed output lifecycle did not reach compiled footer labels");
                if (((Microsoft.UI.Xaml.Controls.TextBlock)page.FindName("ProgramOutputHealth")).Text != "Idle") failures.Add($"{state}: Program health label retained its old text");
                var textBlocks = Descendants((FrameworkElement)page.FindName("FooterTelemetry")).OfType<Microsoft.UI.Xaml.Controls.TextBlock>().ToArray();
                if (!textBlocks.Any(text => text.Text == record.Progress) || !textBlocks.Any(text => text.Text == stream.Progress))
                    failures.Add($"{state}: recording frame progress did not reach the footer");
                await ReviewImageWriter.SaveAsync(page, Path.Combine(directory, $"studio-output-{state}.png"));
            }
            checks.Add("Compiled recording and sender labels/progress follow preparing, producing, interrupted, failed, finalizing, completed and idle despite contradictory active flags");
            var capturedPath = Environment.GetEnvironmentVariable("COREVIDEO_OUTPUT_TELEMETRY_FIXTURES");
            if (!string.IsNullOrWhiteSpace(capturedPath))
            {
                var observed = new List<object>();
                var recordCounts = new HashSet<string>(); var streamCounts = new HashSet<string>();
                foreach (var line in await File.ReadAllLinesAsync(capturedPath))
                {
                    using var response = JsonDocument.Parse("{\"ok\":true,\"snapshot\":" + line + "}");
                    var wire = CoreProtocolParser.TryParseWireState(response) ?? throw new InvalidOperationException("Captured native state did not parse through the production protocol parser");
                    var snapshot = NativeMediaCoreStateMapper.MapNativeWireStateToSnapshot(0, 0, wire);
                    studio.Transport.ApplySnapshot(snapshot, false, false, "1080p60", true);
                    await Task.Delay(10); page.UpdateLayout();
                    var record = StudioOutputTelemetry.Recording(snapshot.Recording);
                    var stream = StudioOutputTelemetry.Streaming(snapshot);
                    if (((Microsoft.UI.Xaml.Controls.TextBlock)page.FindName("RecordOutputHealth")).Text != record.State ||
                        ((Microsoft.UI.Xaml.Controls.TextBlock)page.FindName("StreamOutputHealth")).Text != stream.State)
                        failures.Add("Captured native state did not reach compiled output labels");
                    var labels = Descendants((FrameworkElement)page.FindName("FooterTelemetry")).OfType<Microsoft.UI.Xaml.Controls.TextBlock>().ToArray();
                    if (!labels.Any(text => text.Text == record.Progress) || !labels.Any(text => text.Text == stream.Progress)) failures.Add("Captured native frame counts did not reach compiled output labels");
                    observed.Add(new { recording = record, streaming = stream });
                    if (record.State == "Recording" && stream.State == "Streaming")
                    {
                        recordCounts.Add(record.Progress); streamCounts.Add(stream.Progress);
                        await ReviewImageWriter.SaveAsync(page, Path.Combine(directory, "studio-real-output-producing.png"));
                    }
                }
                if (recordCounts.Count < 2 || streamCounts.Count < 2) failures.Add("Captured writer and local sender did not each show progressing frame counts while producing");
                await File.WriteAllTextAsync(Path.Combine(directory, "rendered-real-output-readouts.json"), JsonSerializer.Serialize(observed, new JsonSerializerOptions { WriteIndented = true }));
                checks.Add("Real isolated native recording and localhost sender snapshots pass through the production parser/mapper into rendered status and progressing frame counts");
            }
            foreach(var (width,height) in new[] {(1904,1000),(1424,860),(1360,768)})
            {
                page.Width=width;page.Height=height;
                studio.ActiveTab=CoreVideoPro.WinUI.Models.StudioTab.Overlays;
                // The show-look cards live in the advanced workspace (Design → Advanced editor, #1076).
                studio.GraphicsMode=StudioViewModel.GraphicsModeDesign;studio.GraphicsDesignAdvanced=true;
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
