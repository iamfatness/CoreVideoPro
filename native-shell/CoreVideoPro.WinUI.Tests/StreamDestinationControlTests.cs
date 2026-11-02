using System.Text.Json;
using CoreVideoPro.Control;
using CoreVideoPro.Control.Http;
using CoreVideoPro.WinUI.Services;
using Xunit;

namespace CoreVideoPro.WinUI.Tests;

public sealed class StreamDestinationControlTests
{
    private static readonly object?[] Rtmp = [true, "rtmps", "rtmps://example.test/live", "test-key-sentinel"];
    private static readonly object?[] Srt = [true, "caller", "example.test", "9000", "120", "test-stream-id", "16", "test-passphrase-sentinel"];

    [Theory]
    [InlineData(false)]
    [InlineData(true)]
    public void BusyOrInvalidRequestsNeverSaveOrApply(bool srt)
    {
        var args = srt ? Srt : Rtmp;
        var current = new ProductionOutputPreferences();
        Assert.False(StreamDestinationControl.Configure(srt, args, true, current,
            _ => Assert.Fail("No save during streaming/transition"), _ => Assert.Fail("No apply")).Ok);
        var invalid = args.ToArray(); invalid[1] = "invalid-mode-test-secret";
        var result = StreamDestinationControl.Configure(srt, invalid, false, current,
            _ => Assert.Fail("No save on validation failure"), _ => Assert.Fail("No apply"));
        Assert.False(result.Ok);
        Assert.DoesNotContain("invalid-mode-test-secret", result.Error);
        Assert.False(StreamDestinationControl.Configure(srt, [], false, current,
            _ => Assert.Fail("No save"), _ => Assert.Fail("No apply")).Ok);
    }

    [Theory]
    [InlineData(false)]
    [InlineData(true)]
    public void StorageFailureKeepsMemoryAndReportsNoSecretAndRetrySucceeds(bool srt)
    {
        var current = new ProductionOutputPreferences { StreamRtmpStreamKey = "original", StreamSrtPassphrase = "original-passphrase" };
        var fail = true;
        ProductionOutputPreferences? saved = null;
        void Save(ProductionOutputPreferences next) { if (fail) throw new IOException("test-key-sentinel test-passphrase-sentinel"); saved = next; }
        var result = StreamDestinationControl.Configure(srt, srt ? Srt : Rtmp, false, current, Save,
            _ => Assert.Fail("Do not apply after failed save"));
        Assert.False(result.Ok);
        Assert.DoesNotContain("sentinel", result.Error);
        Assert.Equal("original", current.StreamRtmpStreamKey);
        Assert.Equal("original-passphrase", current.StreamSrtPassphrase);
        fail = false;
        Assert.True(StreamDestinationControl.Configure(srt, srt ? Srt : Rtmp, false, current, Save,
            next => Assert.Same(saved, next)).Ok);
        Assert.NotNull(saved);
    }

    [Fact]
    public void OmittedSecretsRetainAndExplicitEmptyClearsOnlyWhenDisabled()
    {
        var current = new ProductionOutputPreferences { StreamRtmpStreamKey = "existing-key", StreamSrtPassphrase = "existing-passphrase", NdiOutputEnabled = true };
        ProductionOutputPreferences applied = current;
        Assert.True(StreamDestinationControl.Configure(false, Rtmp[..3], false, current, _ => { }, next => applied = next).Ok);
        Assert.Equal("existing-key", applied.StreamRtmpStreamKey);
        Assert.True(applied.NdiOutputEnabled);
        Assert.True(StreamDestinationControl.Configure(true, Srt[..7], false, current, _ => { }, next => applied = next).Ok);
        Assert.Equal("existing-passphrase", applied.StreamSrtPassphrase);
        Assert.False(StreamDestinationControl.Configure(false, [true, "rtmps", "example.test/live", ""], false, current,
            _ => Assert.Fail("Enabled RTMP needs a key"), _ => { }).Ok);
        Assert.True(StreamDestinationControl.Configure(false, [false, "rtmps", "", ""], false, current, _ => { }, next => applied = next).Ok);
        Assert.Equal("", applied.StreamRtmpStreamKey);
    }

    [Theory]
    [InlineData("host")]
    [InlineData("port")]
    [InlineData("latency")]
    [InlineData("encryption")]
    public void InvalidSrtSettingsAreRejectedBeforeCommit(string field)
    {
        var args = Srt.ToArray();
        args[field switch { "host" => 2, "port" => 3, "latency" => 4, _ => 6 }] =
            field switch { "host" => "srt://host?secret=x", "port" => "65536", "latency" => "19", _ => "15" };
        Assert.False(StreamDestinationControl.Configure(true, args, false, new(),
            _ => Assert.Fail("No save"), _ => Assert.Fail("No apply")).Ok);
    }

    [Fact]
    public async Task HttpManifestAndInvocationPersistEncryptedSettingsWithoutRestartOrSecretFeedback()
    {
        var directory = Path.Combine(Path.GetTempPath(), "CoreVideo-Destination-" + Guid.NewGuid().ToString("N"));
        try
        {
            var store = new FileProductionOutputPreferencesStore(directory,
                protectSecret: DpapiSecretProtector.Protect, unprotectSecret: DpapiSecretProtector.Unprotect);
            var surface = new DestinationSurface(store);
            var router = new HttpControlRouter(surface);
            Assert.Contains("transport.stream.rtmp.configure", (await router.HandleAsync("GET", "/manifest", null)).Body);
            foreach (var srt in new[] { false, true })
            {
                var response = await router.HandleAsync("POST", "/invoke", JsonSerializer.Serialize(new {
                    action = srt ? "transport.stream.srt.configure" : "transport.stream.rtmp.configure", args = srt ? Srt : Rtmp }));
                Assert.Equal(200, response.Status);
                Assert.True(JsonDocument.Parse(response.Body).RootElement.GetProperty("ok").GetBoolean());
                Assert.DoesNotContain("sentinel", response.Body);
            }
            var saved = store.Load()!;
            Assert.Equal("test-key-sentinel", saved.StreamRtmpStreamKey);
            Assert.Equal("test-passphrase-sentinel", saved.StreamSrtPassphrase);
            Assert.Equal("example.test", surface.Current.StreamSrtHost);
            Assert.Equal(2, surface.Applied);
            Assert.False(surface.GetState().Streaming);
            var disk = File.ReadAllText(Path.Combine(directory, FileProductionOutputPreferencesStore.DefaultFileName));
            Assert.Contains("dpapi:", disk);
            Assert.DoesNotContain("sentinel", disk);
            Assert.DoesNotContain("test-stream-id", disk);
        }
        finally
        {
            // Unique, owned test directory; never touches the operator profile.
            if (Directory.Exists(directory)) Directory.Delete(directory, true);
        }
    }

    private sealed class DestinationSurface(IProductionOutputPreferencesStore store) : IControlSurface
    {
        internal ProductionOutputPreferences Current = new() { NdiOutputEnabled = true };
        internal int Applied;
        public Task<ControlInvokeResult> InvokeAsync(string actionId, IReadOnlyList<object?> args, CancellationToken cancellationToken = default) =>
            Task.FromResult(StreamDestinationControl.Configure(actionId == "transport.stream.srt.configure", args, false,
                Current, store.Save, next => { Current = next; Applied++; }));
        public ControlState GetState() => ControlState.Empty;
        public event EventHandler<ControlState>? StateChanged { add { } remove { } }
    }
}
