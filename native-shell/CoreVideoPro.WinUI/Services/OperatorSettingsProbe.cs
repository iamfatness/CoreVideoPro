using System.Diagnostics;
using System.Text.Json;
using CoreVideoPro.MediaCore.Services;
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
            var health = Descendants(page).OfType<Expander>().Single(e => e.Name == "HealthSection");
            health.IsExpanded = true;
            await Eventually(() => Descendants(health).OfType<DiagnosticsView>().Any(v => v.IsLoaded));
            Require(Descendants(health).OfType<Button>().Any(b => b.Content?.ToString() == "Export support bundle"), "Support export is missing from Settings health");
            checks.Add("Settings health expands with the shared diagnostics and support export surface");
            window.AppWindow.Resize(new SizeInt32(900, 640));
            health.IsExpanded = false;
            checks.Add("Settings health collapse and narrow window resize");
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
