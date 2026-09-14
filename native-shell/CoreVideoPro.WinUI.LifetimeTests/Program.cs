using System.Reflection;
using CoreVideoPro.WinUI.Controls;
using Microsoft.UI.Dispatching;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Media;
using Windows.Foundation;

// Run on a real WinUI dispatcher, without opening a window or touching the
// operator's running application. This tests reuse, not reproduction of #513.
internal static class Program
{
    private const BindingFlags PrivateInstance = BindingFlags.Instance | BindingFlags.NonPublic;
    private static int _exitCode = 1;

    [STAThread]
    private static int Main()
    {
        WinRT.ComWrappersSupport.InitializeComWrappers();
        Application.Start(_ =>
        {
            SynchronizationContext.SetSynchronizationContext(
                new DispatcherQueueSynchronizationContext(DispatcherQueue.GetForCurrentThread()));
            var app = new Application();
            DispatcherQueue.GetForCurrentThread().TryEnqueue(async () =>
            {
                try
                {
                    await RunAsync();
                    Console.WriteLine("PASS: meter identity, bounded pools, layout, fill, mute, and unloaded timer checks");
                    _exitCode = 0;
                }
                catch (Exception ex)
                {
                    Console.Error.WriteLine(ex);
                }
                finally { app.Exit(); }
            });
        });
        return _exitCode;
    }

    private static async Task RunAsync()
    {
        var meter = new AudioLevelMeter { SegmentCount = 48, IsVertical = true, ShowDbfsScale = true };
        var root = (Grid)meter.Content;
        var panel = (StackPanel)root.Children[0];
        var scale = (Canvas)root.Children[1];
        var render = typeof(AudioLevelMeter).GetMethod("RenderSegments", PrivateInstance)!;
        void Render(double width, double height)
        {
            meter.Measure(new Size(width, height));
            meter.Arrange(new Rect(0, 0, width, height));
            render.Invoke(meter, null);
        }
        Render(80, 600);
        Check(panel.Children.Count == 48, "48-segment warmup");
        Check(scale.Children.Count == 14, "seven scale marks");
        var segments = panel.Children.ToArray();
        var marks = scale.Children.ToArray();

        for (var i = 0; i < 10000; i++)
        {
            meter.IsVertical = i % 2 == 0;
            meter.ShowDbfsScale = i % 3 != 0;
            meter.SegmentCount = 8 + i % 41;
            meter.IsMuted = i % 5 == 0;
            meter.ShowLevelWhileMuted = i % 7 == 0;
            meter.Level = i % 101;
            Render(80 + i % 300, new double[] { 40, 80, 600 }[i % 3]);
            Check(ReferenceEquals(root, meter.Content), "root retained");
            Check(panel.Children.Count == 48 && scale.Children.Count == 14, "bounded pools");
            Check(segments.SequenceEqual(panel.Children), "segment identities retained");
            Check(marks.SequenceEqual(scale.Children), "scale identities retained");
            Check(panel.Orientation == (meter.IsVertical ? Orientation.Vertical : Orientation.Horizontal), "orientation");
            Check(typeof(AudioLevelMeter).GetField("_decayTimer", PrivateInstance)!.GetValue(meter) is null,
                "level changes cannot start unloaded timer");
        }

        meter.IsVertical = true;
        meter.ShowDbfsScale = true;
        meter.SegmentCount = 36;
        foreach (var height in new[] { 40d, 80d, 600d })
        {
            Render(80, height);
            var expected = height < 60 ? new[] { "0", "-30", "-60" }
                : height < 100 ? new[] { "0", "-12", "-24", "-36", "-48", "-60" }
                : new[] { "0", "-6", "-12", "-24", "-36", "-48", "-60" };
            Check(scale.Children.OfType<TextBlock>().Where(x => x.Visibility == Visibility.Visible)
                .Select(x => x.Text).SequenceEqual(expected), "responsive scale labels");
            var visible = panel.Children.OfType<Border>().Where(x => x.Visibility == Visibility.Visible).ToArray();
            Check(Math.Abs(visible.Sum(x => x.Height) + panel.Spacing * (visible.Length - 1) - height) < 0.01,
                "segments fit available height");
        }

        meter.IsVertical = false;
        meter.IsMuted = false;
        meter.Level = 100;
        Render(600, 40);
        var first = (Border)panel.Children[0];
        Check(((SolidColorBrush)first.Background).Color.G == 210, "green low segment");
        meter.IsMuted = true;
        meter.ShowLevelWhileMuted = false;
        Render(600, 40);
        Check(((SolidColorBrush)first.Background).Color.G == 30, "mute immediately darkens meter");
        meter.ShowLevelWhileMuted = true;
        Render(600, 40);
        Check(((SolidColorBrush)first.Background).Color.G == 122, "pre-mute input color");
        Check(scale.Visibility == Visibility.Collapsed, "horizontal scale hidden");
        Check(Grid.GetColumnSpan(panel) == 2, "unscaled meter spans host");

        // Leave the UI dispatcher free while finalizers run; blocking it can
        // deadlock ordinary apartment-marshaled releases and is not a valid test.
        await Task.Run(() => { GC.Collect(); GC.WaitForPendingFinalizers(); GC.Collect(); });
        Render(600, 40);
        Check(segments.SequenceEqual(panel.Children), "control remains usable after full GC");
        GC.KeepAlive(meter);
    }

    private static void Check(bool condition, string message)
    {
        if (!condition) throw new InvalidOperationException(message);
    }
}
