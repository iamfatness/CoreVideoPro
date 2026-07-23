using CoreVideoPro.MediaCore.Models;
using CoreVideoPro.MediaCore.Services;
using CoreVideoPro.WinUI.Services;
using Xunit;

namespace CoreVideoPro.WinUI.Tests;

/// <summary>
/// The consent gate (spec §S3 / §7): nothing is transmitted unless the toggle is
/// ON *and* the endpoint/key are configured. Uses an injected fake transport so no
/// real HTTP happens; the session-end path is fire-and-forget, so the fake signals
/// a TaskCompletionSource we can await (or assert stays unset for the OFF case).
/// </summary>
public sealed class TelemetryEventServiceTests : IDisposable
{
    private readonly string _dir = Directory.CreateTempSubdirectory("cvp-telemetry-svc-").FullName;

    private string ConsentPath => Path.Combine(_dir, "telemetry-consent.json");

    public void Dispose()
    {
        try { Directory.Delete(_dir, recursive: true); } catch { /* best effort */ }
    }

    private static readonly CrashReportingConfig Configured =
        new() { Endpoint = "https://telemetry.example", ApiKey = "test-key" };

    private sealed class FakeTransport
    {
        public int Calls;
        public string? LastWire;
        public Uri? LastEndpoint;
        public readonly TaskCompletionSource Called = new(TaskCreationOptions.RunContinuationsAsynchronously);

        public Task<TelemetrySendResult> SendAsync(Uri endpoint, string apiKey, string wire, CancellationToken ct)
        {
            Interlocked.Increment(ref Calls);
            LastWire = wire;
            LastEndpoint = endpoint;
            Called.TrySetResult();
            return Task.FromResult(new TelemetrySendResult
            {
                Status = TelemetrySendStatus.Accepted,
                ReportId = "cv-test-1",
                HttpStatus = 202
            });
        }
    }

    private TelemetryEventService NewService(
        FakeTransport transport,
        CrashReportingConfig? config = null,
        NativeMediaCoreStateSnapshot? snapshot = null)
    {
        return new TelemetryEventService(
            snapshotAccessor: () => snapshot,
            consentStore: new TelemetryConsentStore(ConsentPath),
            config: config ?? Configured,
            crashWatermarkAccessor: () => null,
            versionAccessor: () => "1.2.3",
            gpuTierProbe: () => null,
            transport: transport.SendAsync);
    }

    [Fact]
    public async Task ConsentOff_FireSessionEnd_SendsNothing()
    {
        // No consent file at all = OFF (fresh profile).
        var transport = new FakeTransport();
        using var service = NewService(transport);
        service.Start();

        service.FireSessionEnd();

        // Give any (erroneously) spawned send time to run — it must not.
        await Task.Delay(300);
        Assert.Equal(0, transport.Calls);
    }

    [Fact]
    public async Task ConsentOn_ButUnconfigured_SendsNothing()
    {
        new TelemetryConsentStore(ConsentPath).SetEnabled(true);
        var transport = new FakeTransport();
        // Empty endpoint/key => IsConfigured false.
        using var service = NewService(transport, config: new CrashReportingConfig());
        service.Start();

        Assert.False(service.IsConfigured);
        service.FireSessionEnd();

        await Task.Delay(300);
        Assert.Equal(0, transport.Calls);
    }

    [Fact]
    public async Task ConsentOn_AndConfigured_SendsSessionEnd()
    {
        new TelemetryConsentStore(ConsentPath).SetEnabled(true);
        var transport = new FakeTransport();
        using var service = NewService(transport);
        service.Start();

        service.FireSessionEnd();

        await transport.Called.Task.WaitAsync(TimeSpan.FromSeconds(5));
        Assert.Equal(1, transport.Calls);
        Assert.Equal("/v1/events", new Uri(transport.LastEndpoint!, "/v1/events").AbsolutePath);
        Assert.Contains("\"name\":\"session-end\"", transport.LastWire);
        Assert.Contains("\"version\":\"1.2.3\"", transport.LastWire);
        Assert.Contains("outputConfigShape", transport.LastWire);

        // Accepted send updates the last-sent watermark (crash-count baseline).
        // MarkSent runs just AFTER the transport returns, so poll briefly.
        DateTimeOffset? lastSent = null;
        for (var i = 0; i < 50 && lastSent is null; i++)
        {
            lastSent = new TelemetryConsentStore(ConsentPath).Load().LastSentUtc;
            if (lastSent is null)
            {
                await Task.Delay(20);
            }
        }

        Assert.NotNull(lastSent);
    }

    [Fact]
    public async Task FireSessionEnd_IsIdempotent_SendsOnce()
    {
        new TelemetryConsentStore(ConsentPath).SetEnabled(true);
        var transport = new FakeTransport();
        using var service = NewService(transport);
        service.Start();

        service.FireSessionEnd();
        service.FireSessionEnd();
        service.FireSessionEnd();

        await transport.Called.Task.WaitAsync(TimeSpan.FromSeconds(5));
        await Task.Delay(200);
        Assert.Equal(1, transport.Calls);
    }

    [Fact]
    public void BuildPreviewJson_WorksEvenWhenDisabled()
    {
        // Preview must work with consent OFF so the operator can audit BEFORE opting in.
        var transport = new FakeTransport();
        using var service = NewService(transport);
        service.Start();

        var json = service.BuildPreviewJson();

        Assert.Contains("\"name\": \"session-end\"", json);
        Assert.Contains("outputConfigShape", json);
        Assert.Contains("machineClass", json);
        Assert.Equal(0, transport.Calls); // building a preview never sends
    }
}
