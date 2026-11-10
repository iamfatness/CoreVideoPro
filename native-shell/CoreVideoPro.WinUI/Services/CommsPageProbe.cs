using System.Text.Json;
using CoreVideoPro.MediaCore.Models;
using CoreVideoPro.MediaCore.Services;
using CoreVideoPro.WinUI.Controls;
using CoreVideoPro.WinUI.ViewModels.Comms;
using CoreVideoPro.WinUI.Views;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Media;
using Windows.Graphics;

namespace CoreVideoPro.WinUI.Services;

/// <summary>
/// Compiled-XAML check of the Comms page (docs/reference/comms-talkback.md), run by
/// <c>CoreVideoPro.WinUI.exe --verify-comms-page &lt;report.json&gt;</c>. Same rules as the other
/// layout probes: an offscreen, never-activated window; no media core, no meeting, no input
/// injection. Feeds the real <see cref="CommsPage"/> fixture states through a recording
/// transport and writes review PNGs next to the report.
/// </summary>
internal static class CommsPageProbe
{
    private sealed class RecordingTransport : IZoomCommsTransport
    {
        public event Action<ZoomCommsUpdate>? ZoomCommsStateReceived { add { } remove { } }
        public List<string> Sent { get; } = new();
        public Task<ZoomCommsCommandResult> SendCommsAsync(string verb, IReadOnlyDictionary<string, object?> payload,
            CancellationToken cancellationToken = default)
        {
            Sent.Add(verb + " " + JsonSerializer.Serialize(payload));
            return Task.FromResult(ZoomCommsCommandResult.Success);
        }
    }

    private const string Desk = """
        {"available":true,"reason":null,"lamps":{"link":true,"meetingMic":true,"channel":true,"tx":true},"room":null,
         "channels":[
          {"slot":0,"label":"Ana Lucia Ferreira","listeners":1,"ready":true,"keyed":true,"latched":false,"room":null,"reachOk":1,"reachDark":0},
          {"slot":1,"label":"Ben","listeners":1,"ready":true,"keyed":false,"latched":false,"room":null,"reachOk":1,"reachDark":0},
          {"slot":2,"label":"Chloe","listeners":0,"ready":true,"keyed":true,"latched":true,"room":null,"reachOk":0,"reachDark":0},
          {"slot":3,"label":"Dev","listeners":1,"ready":true,"keyed":false,"latched":false,"room":"Green room","reachOk":0,"reachDark":1},
          {"slot":4,"label":null,"listeners":3,"ready":true,"keyed":false,"latched":false,"room":null,"reachOk":3,"reachDark":1},
          {"slot":5,"label":"Eve","listeners":0,"ready":false,"keyed":false,"latched":false,"room":null,"reachOk":0,"reachDark":0},
          {"slot":6,"label":null,"listeners":0,"ready":true,"keyed":false,"latched":false,"room":null,"reachOk":0,"reachDark":0}],
         "roster":[
          {"participantId":201,"name":"Ana Lucia Ferreira","talkback":true,"room":null,"channels":[0,4]},
          {"participantId":202,"name":"Ben","talkback":true,"room":null,"channels":[1,4]},
          {"participantId":205,"name":"Wes","talkback":false,"room":null,"channels":[]}],
         "mic":{"device":"Microphone (USB Audio)","devices":["Microphone (USB Audio)","Headset Mic"],"peakDbfs":-18,"gainDb":3,
                "sidetone":true,"sidetoneDevice":"Headphones (USB Audio)","sidetoneDevices":["Headphones (USB Audio)","Speakers"],"tone":false},
         "stats":{"framesSent":51200,"channelSends":8800,"underruns":0,"sendErrors":2},
         "log":["21:40:02 requesting 16 talkback channels","21:40:03 station up -- 16 channels","21:40:08 Ana Lucia Ferreira -> CH 1","21:40:15 audio flowing into CH 1"]}
        """;

    internal static async Task CheckAsync(Window window, List<string> checks, string reviewDirectory)
    {
        var transport = new RecordingTransport();
        var vm = new CommsViewModel(transport);
        var page = new CommsPage { ViewModel = vm };
        var host = new Border
        {
            Padding = new Thickness(20),
            Background = (Brush)Application.Current.Resources["StudioPanelBrush"],
            Child = page,
        };
        window.Content = host;
        window.AppWindow.MoveAndResize(new RectInt32(-20000, -20000, 1360, 768));
        window.AppWindow.Show(false);
        await Eventually(() => page.IsLoaded);

        vm.ApplyMeeting(false, null);
        await Settle();
        Require(Visible(page, "Not in a meeting"), "Not-in-meeting state is not shown");
        await ReviewImageWriter.SaveAsync(host, Path.Combine(reviewDirectory, "comms-not-in-meeting.png"));
        checks.Add("not in a meeting: explicit state, no desk");

        vm.ApplyMeeting(true, "Meeting 123 456 7890");
        vm.ApplyComms(Parse("""{"available":false,"reason":"Make this account host or co-host to bring talkback up.","lamps":{"link":true,"meetingMic":false,"channel":false,"tx":false}}"""));
        await Settle();
        Require(Visible(page, "Talkback is not up"), "Unavailable state is not shown");
        Require(Visible(page, "Make this account host or co-host to bring talkback up."), "Verbatim reason is not shown");
        await ReviewImageWriter.SaveAsync(host, Path.Combine(reviewDirectory, "comms-not-host.png"));
        checks.Add("not host: verbatim reason, host hint, lamps still truthful");

        vm.ApplyComms(Parse(Desk));
        await Settle();
        var keys = Descendants(page).OfType<TalkKey>().Where(k => k.Tag is CommsCellViewModel && k.ActualWidth > 0).ToList();
        Require(keys.Count == 6, $"Expected 6 talk keys (occupied/named/keyed channels), found {keys.Count}");
        Require(keys.All(k => k.ActualWidth >= 190), "A talk key is narrower than the 200 px auto-fill minimum");
        var hot = keys.Single(k => ((CommsCellViewModel)k.Tag).Slot == 0);
        Require(ReferenceEquals(hot.Background, Application.Current.Resources["StudioAirBrush"]), "On-air key is not StudioAirBrush");
        var armed = keys.Single(k => ((CommsCellViewModel)k.Tag).Slot == 2);
        Require(ReferenceEquals(armed.BorderBrush, Application.Current.Resources["StudioWarningBrush"]), "Armed key is not outlined StudioWarningBrush");
        Require(!ReferenceEquals(armed.Background, Application.Current.Resources["StudioAirBrush"]), "A key with nobody listening shows on air");
        checks.Add("desk: 6 keys in an auto-fill grid; on air = StudioAirBrush, armed = warning outline, never red without a listener");
        await ReviewImageWriter.SaveAsync(host, Path.Combine(reviewDirectory, "comms-desk.png"));

        vm.EditMode = true;
        vm.SettingsOpen = true;
        vm.OpsOpen = true;
        await Settle();
        Require(Visible(page, "NO TALKBACK"), "The web-client person is not marked");
        var chips = Descendants(page).OfType<Button>().Count(b => b.Width == 32 && b.ActualWidth > 0);
        Require(chips == 14, $"Expected one chip per channel for each of 2 eligible people (14), found {chips}");
        await ReviewImageWriter.SaveAsync(host, Path.Combine(reviewDirectory, "comms-edit-settings.png"));
        checks.Add("edit talent: one chip per channel per eligible person, web client explained; settings drawer and ops history open");

        Require(transport.Sent.Count == 0, "Rendering states sent a command: " + string.Join("; ", transport.Sent));
        checks.Add("applying and rendering state never sends a Comms command");
    }

    private static ZoomCommsUpdate Parse(string json)
    {
        using var doc = JsonDocument.Parse(json);
        return new ZoomCommsUpdate(ZoomCommsProtocol.ParseState(doc.RootElement));
    }

    private static bool Visible(DependencyObject root, string text) =>
        Descendants(root).OfType<TextBlock>().Any(t => t.Text == text && t.ActualWidth > 0);

    private static async Task Settle() => await Task.Delay(750);

    private static IEnumerable<DependencyObject> Descendants(DependencyObject root)
    {
        var count = VisualTreeHelper.GetChildrenCount(root);
        for (var i = 0; i < count; i++)
        {
            var child = VisualTreeHelper.GetChild(root, i);
            yield return child;
            foreach (var nested in Descendants(child)) yield return nested;
        }
    }

    private static async Task Eventually(Func<bool> ready)
    {
        for (var i = 0; i < 200 && !ready(); i++) await Task.Delay(25);
        if (!ready()) throw new TimeoutException("Comms page did not load.");
    }

    private static void Require(bool condition, string message) { if (!condition) throw new InvalidOperationException(message); }
}
