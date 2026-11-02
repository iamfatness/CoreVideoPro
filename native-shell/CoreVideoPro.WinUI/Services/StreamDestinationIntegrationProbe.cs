using System.Text.Json;
using CoreVideoPro.Control.Http;
using CoreVideoPro.WinUI.ViewModels;
using Microsoft.UI.Dispatching;

namespace CoreVideoPro.WinUI.Services;

// Isolated real dispatcher -> control adapter -> VM -> preference store qualification.
// No window activation, core, media devices, HTTP listener, meeting or production profile.
internal static class StreamDestinationIntegrationProbe
{
    private sealed class Store : IProductionOutputPreferencesStore
    {
        internal ProductionOutputPreferences Value = new() { NdiOutputEnabled = true };
        internal int Saves;
        internal bool Fail;
        public ProductionOutputPreferences? Load() => Value;
        public void Save(ProductionOutputPreferences next)
        {
            if (Fail) throw new IOException("synthetic-key-sentinel");
            Value = next; Saves++;
        }
    }

    internal static async Task CheckAsync(List<string> checks)
    {
        var store = new Store();
        await using var vm = new StudioViewModel(StudioStartupBootstrap.Create(store, startInteractiveServices: false));
        using var surface = new StudioControlSurface(vm, DispatcherQueue.GetForCurrentThread());
        var router = new HttpControlRouter(surface);
        var rtmp = new object?[] { true, "rtmps", "rtmps://example.test/live", "synthetic-key-sentinel" };
        async Task<HttpControlResponse> Invoke(bool srt, object?[] args) => await router.HandleAsync("POST", "/invoke",
            JsonSerializer.Serialize(new { action = srt ? "transport.stream.srt.configure" : "transport.stream.rtmp.configure", args }));
        void Require(bool ok, string message) { if (!ok) throw new InvalidOperationException(message); }

        var before = store.Saves;
        var result = await Invoke(false, rtmp);
        Require(result.Status == 200 && store.Saves == before + 1, "RTMP did not commit exactly once through the real adapter");
        Require(vm.StreamRtmpServerUrl == "rtmps://example.test/live" && vm.StreamRtmpStreamKey == "synthetic-key-sentinel", "RTMP did not apply immediately to the VM");
        Require(!vm.StreamingRequested && !vm.Streaming && vm.NdiOutputEnabled, "Configuration changed streaming intent or another destination");
        checks.Add("HTTP router -> real UI-dispatched adapter -> VM commits RTMP exactly once, updates live settings, preserves other destinations and does not start streaming");

        store.Fail = true;
        var changed = rtmp.ToArray(); changed[2] = "rtmps://other.example.test/live";
        result = await Invoke(false, changed);
        Require(result.Status == 422 && !result.Body.Contains("sentinel") && vm.StreamRtmpServerUrl == "rtmps://example.test/live", "Failed durable save leaked a credential or changed the VM");
        store.Fail = false;
        Require((await Invoke(false, changed)).Status == 200, "Identical retry did not recover");
        checks.Add("Durable-save refusal retains current UI settings, omits exception credentials, and identical retry recovers without restart");

        vm.StreamingRequested = true;
        before = store.Saves;
        Require((await Invoke(false, rtmp)).Status == 422 && store.Saves == before, "Starting stream allowed a destination edit");
        vm.StreamingRequested = false;
        vm.Streaming = true;
        Require((await Invoke(false, rtmp)).Status == 422 && store.Saves == before, "Observed active stream allowed a destination edit");
        vm.Streaming = false;
        checks.Add("Real VM requested-start and observed-active states refuse destination changes before persistence");

        before = store.Saves;
        result = await Invoke(true, [true, "caller", "example.test", "9000", "120", "synthetic-stream-id", "16", "synthetic-passphrase"]);
        Require(result.Status == 200 && store.Saves == before + 1 && vm.StreamSrtHost == "example.test" && vm.StreamSrtPassphrase == "synthetic-passphrase", "SRT did not apply atomically through the real adapter");
        Require(store.Value.StreamRtmpServerUrl == vm.StreamRtmpServerUrl && store.Value.StreamSrtHost == vm.StreamSrtHost, "Next stream start would read different settings than those saved");
        Require(!vm.StreamingRequested && !vm.Streaming, "SRT configuration started streaming");
        checks.Add("SRT destination commits once and updates the settings consumed by the next stream start without a core, meeting or app restart");
    }
}
