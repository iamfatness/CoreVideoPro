using CoreVideoPro.MediaCore.Services;
using Xunit;

namespace CoreVideoPro.MediaCore.Tests;

/// <summary>
/// The standalone consent flag file (spec §S3): a fresh profile is OFF (sends
/// nothing), the toggle persists, and crash-count deltas read the S1 watermark.
/// </summary>
public sealed class TelemetryConsentStoreTests : IDisposable
{
    private readonly string _dir = Directory.CreateTempSubdirectory("cvp-telemetry-consent-").FullName;

    private string Path_ => Path.Combine(_dir, "telemetry-consent.json");

    public void Dispose()
    {
        try { Directory.Delete(_dir, recursive: true); } catch { /* best effort */ }
    }

    [Fact]
    public void FreshProfile_IsDisabled_SendsNothing()
    {
        var store = new TelemetryConsentStore(Path_);
        Assert.False(File.Exists(Path_));
        Assert.False(store.Load().Enabled);
        Assert.False(store.IsEnabled());
    }

    [Fact]
    public void SetEnabled_Persists()
    {
        var store = new TelemetryConsentStore(Path_);
        store.SetEnabled(true);

        // A fresh instance re-reads the flag from disk.
        Assert.True(new TelemetryConsentStore(Path_).IsEnabled());

        store.SetEnabled(false);
        Assert.False(new TelemetryConsentStore(Path_).IsEnabled());
    }

    [Fact]
    public void MarkSent_PreservesConsentAndRecordsWatermark()
    {
        var store = new TelemetryConsentStore(Path_);
        store.SetEnabled(true);
        var when = DateTimeOffset.UtcNow;
        store.MarkSent(when);

        var reloaded = new TelemetryConsentStore(Path_).Load();
        Assert.True(reloaded.Enabled);
        Assert.NotNull(reloaded.LastSentUtc);
        Assert.True(Math.Abs((reloaded.LastSentUtc!.Value - when).TotalSeconds) < 2);
    }

    [Fact]
    public void CorruptFile_DegradesToDisabled()
    {
        File.WriteAllText(Path_, "{ this is not valid json");
        Assert.False(new TelemetryConsentStore(Path_).IsEnabled());
    }

    [Fact]
    public void CrashCount_CountsReportsSinceLastSend()
    {
        var watermark = new CrashReportWatermark
        {
            Reports =
            [
                new CrashReportRecord { ReportId = "a", SentAtUtc = DateTimeOffset.UtcNow.AddHours(-3) },
                new CrashReportRecord { ReportId = "b", SentAtUtc = DateTimeOffset.UtcNow.AddHours(-1) },
                new CrashReportRecord { ReportId = "c", SentAtUtc = DateTimeOffset.UtcNow }
            ]
        };

        // Never sent: all reports count.
        Assert.Equal(3, TelemetryCrashCount.CountSince(watermark, null));
        // Sent 2h ago: only the two newer reports count.
        Assert.Equal(2, TelemetryCrashCount.CountSince(watermark, DateTimeOffset.UtcNow.AddHours(-2)));
        // Sent just now: nothing newer.
        Assert.Equal(0, TelemetryCrashCount.CountSince(watermark, DateTimeOffset.UtcNow.AddSeconds(1)));
        // No watermark at all.
        Assert.Equal(0, TelemetryCrashCount.CountSince(null, null));
    }
}
