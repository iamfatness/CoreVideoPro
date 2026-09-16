using System.Diagnostics;
using System.Text.Json;
using CoreVideoPro.WinUI.Controls;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Windows.Graphics;

namespace CoreVideoPro.WinUI.Services;

/// <summary>
/// Opt-in, isolated real-XAML regression host. Never constructs the production
/// window, connects to media/Zoom, or reads/writes an operator's show settings.
/// A native fail-fast produces no success report; the runner also checks exit code.
/// </summary>
internal sealed class AudioMeterStressProbe(int seconds, string reportPath)
{
    private Window? _window;

    internal async Task RunAsync()
    {
        using var stopGc = new CancellationTokenSource();
        Task? gcTask = null;
        var elapsed = Stopwatch.StartNew();
        var frames = 0;
        var unloads = 0;
        var collections = 0;
        var animationChecks = 0;
        var destroyedMetersCollected = 0;
        var passed = false;
        string? error = null;
        try
        {
            _window = new Window { Title = "CoreVideo meter stability probe" };
            _window.AppWindow.MoveAndResize(new RectInt32(-20000, -20000, 900, 700));
            var root = new Grid();
            for (var i = 0; i < 4; i++)
            {
                root.RowDefinitions.Add(new RowDefinition());
                root.ColumnDefinitions.Add(new ColumnDefinition());
            }
            var meters = new AudioLevelMeter[16];
            for (var i = 0; i < meters.Length; i++)
            {
                var meter = new AudioLevelMeter { Width = 160, Height = 129, IsVertical = i % 2 == 0,
                    SegmentCount = 24, ShowDbfsScale = true };
                Grid.SetRow(meter, i / 4);
                Grid.SetColumn(meter, i % 4);
                root.Children.Add(meter);
                meters[i] = meter;
            }
            _window.Content = root;
            // Show without activation, outside the desktop viewport. XAML still
            // performs real layout/Loaded/Unloaded work on its dispatcher.
            _window.AppWindow.Show(false);
            await Task.Delay(200);
            Require(meters.All(m => m.IsLoaded), "Probe meters did not load");
            var trees = meters.Select(CaptureTree).ToArray();

            // Mute semantics and the retained pool are checked on actual controls.
            var first = meters[0];
            first.Level = 100;
            var panel = (StackPanel)((Grid)first.Content).Children[0];
            var lit = ((Border)panel.Children[0]).Background;
            first.IsMuted = true;
            var dim = ((Border)panel.Children[0]).Background;
            Require(!ReferenceEquals(lit, dim), "Output mute did not darken meter");
            Require(!first.AnimationRunning, "Output mute left animation running");
            first.ShowLevelWhileMuted = true;
            var input = ((Border)panel.Children[0]).Background;
            Require(!ReferenceEquals(input, dim) && !ReferenceEquals(input, lit), "Muted input is not visually distinct");

            gcTask = Task.Run(async () =>
            {
                while (!stopGc.IsCancellationRequested)
                {
                    GC.Collect(2, GCCollectionMode.Forced, blocking: true, compacting: true);
                    GC.WaitForPendingFinalizers();
                    Interlocked.Increment(ref collections);
                    try { await Task.Delay(250, stopGc.Token); }
                    catch (OperationCanceledException) { break; }
                }
            });
            while (elapsed.Elapsed.TotalSeconds < seconds)
            {
                for (var i = 0; i < meters.Length; i++)
                {
                    var meter = meters[i];
                    meter.IsMuted = (frames + i) % 11 == 0;
                    meter.ShowLevelWhileMuted = (frames + i) % 3 == 0;
                    meter.Level = frames % 97 == 0 ? double.NaN : (frames * 13 + i * 17) % 101;
                    if (frames % 10 == 0)
                    {
                        meter.Width = 1 + (frames * 7 + i * 31) % 180;
                        meter.Height = 1 + (frames * 11 + i * 23) % 160;
                        meter.IsVertical = (frames / 40 + i) % 2 == 0;
                        meter.ShowDbfsScale = (frames / 30 + i) % 2 == 0;
                        meter.SegmentCount = (frames / 10 + i) % 4 == 0 ? int.MaxValue : 8 + i * 3;
                    }
                    VerifyTree(meter, trees[i]);
                    if (meter.AnimationRunning) animationChecks++;
                }
                if (frames % 60 == 59)
                {
                    // Real unload/reload, plus binding updates while detached.
                    _window.Content = new Grid();
                    await Task.Delay(40);
                    foreach (var meter in meters)
                    {
                        Require(!meter.IsLoaded, "Detached meter stayed loaded");
                        meter.Level = 100;
                        meter.Level = 0;
                        Require(!meter.AnimationRunning, "Unloaded meter restarted timer");
                    }
                    _window.Content = root;
                    await Task.Delay(40);
                    Require(meters.All(m => m.IsLoaded), "Meter did not reload");
                    unloads++;
                }
                frames++;
                await Task.Delay(16);
            }
            Require(animationChecks > 0 && unloads > 0, "Animation/lifecycle paths were not exercised");
            foreach (var meter in meters) { meter.IsMuted = false; meter.Level = 0; }
            await Task.Delay(2500);
            Require(meters.All(m => !m.AnimationRunning), "Silent meters did not become idle");
            for (var i = 0; i < meters.Length; i++) VerifyTree(meters[i], trees[i]);

            // Release entire meter trees too, so page destruction—not only
            // retained-page navigation—runs through the real WinRT finalizers.
            _window.Content = null;
            await Task.Delay(50);
            Require(meters.All(m => !m.AnimationRunning), "Shutdown left timers running");
            var discarded = new List<WeakReference>();
            for (var i = 0; i < 20; i++) discarded.Add(await CreateDiscardedMeterAsync());
            // Allow XAML reference tracking and UI-affine release work to run
            // between collections. Never wait for finalizers on the UI thread.
            for (var i = 0; i < 5; i++)
            {
                await Task.Run(() => { GC.Collect(); GC.WaitForPendingFinalizers(); });
                await Task.Delay(100);
            }
            destroyedMetersCollected = discarded.Count(reference => !reference.IsAlive);
            Require(destroyedMetersCollected == discarded.Count, "Discarded meter was retained after unload/GC");
            passed = true;
        }
        catch (Exception ex)
        {
            error = ex.ToString();
        }
        finally
        {
            stopGc.Cancel();
            if (gcTask is not null) await gcTask;
            var report = new { passed, error, frames, unloads, collections, animationChecks, destroyedMetersCollected,
                elapsedSeconds = elapsed.Elapsed.TotalSeconds, processId = Environment.ProcessId };
            Directory.CreateDirectory(Path.GetDirectoryName(Path.GetFullPath(reportPath))!);
            await File.WriteAllTextAsync(reportPath, JsonSerializer.Serialize(report, new JsonSerializerOptions { WriteIndented = true }));
            Environment.ExitCode = passed ? 0 : 1;
            _window?.Close();
        }
    }

    private async Task<WeakReference> CreateDiscardedMeterAsync()
    {
        var meter = new AudioLevelMeter { Width = 80, Height = 129, IsVertical = true, ShowDbfsScale = true };
        _window!.Content = meter;
        await Task.Delay(40);
        Require(meter.IsLoaded, "Destruction-test meter did not load");
        meter.Level = 100;
        meter.Level = 0;
        Require(meter.AnimationRunning, "Destruction-test timer did not start");
        _window.Content = null;
        await Task.Delay(40);
        Require(!meter.IsLoaded && !meter.AnimationRunning, "Destroyed page retained its meter timer");
        return new WeakReference(meter);
    }

    private static DependencyObject[] CaptureTree(AudioLevelMeter meter)
    {
        var root = (Grid)meter.Content;
        var panel = (StackPanel)root.Children[0];
        var scale = (Canvas)root.Children[1];
        return new DependencyObject[] { root, panel, scale }
            .Concat(panel.Children.Cast<DependencyObject>()).Concat(scale.Children.Cast<DependencyObject>()).ToArray();
    }

    private static void VerifyTree(AudioLevelMeter meter, DependencyObject[] original)
    {
        var current = CaptureTree(meter);
        Require(current.Length == 67 && current.Length == original.Length, "Meter visual pool is not bounded");
        for (var i = 0; i < current.Length; i++)
            Require(ReferenceEquals(current[i], original[i]), "Meter replaced a retained visual");
        var panel = (StackPanel)current[1];
        var visible = panel.Children.Cast<Border>().Where(b => b.Visibility == Visibility.Visible).ToArray();
        Require(visible.Length is > 0 and <= 48, "Invalid visible segment count");
        Require(visible.All(b => double.IsFinite(b.Width) && double.IsFinite(b.Height)), "Invalid meter geometry");
    }

    private static void Require(bool condition, string message)
    {
        if (!condition) throw new InvalidOperationException(message);
    }
}
