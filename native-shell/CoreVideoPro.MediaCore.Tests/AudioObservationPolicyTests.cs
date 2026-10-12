using CoreVideoPro.MediaCore.Models;
using CoreVideoPro.MediaCore.Services;
using Xunit;
using System.Text.Json;

namespace CoreVideoPro.MediaCore.Tests;

public sealed class AudioObservationPolicyTests
{
    [Fact]
    public void WireEnvelopeSurvivesParserMapperAndSupportProjection()
    {
        using var response = JsonDocument.Parse("""
            {"ok":true,"snapshot":{"health":null,"profile":null,"sceneId":"program",
             "appliedControlRevision":9,
             "audioObservation":{"epoch":"core-a","generation":1,"sequence":4,"observedRevision":7,
               "ageMs":22,"status":"fresh","refused":2,"capacity":4}}}
            """);
        var wire = CoreProtocolParser.TryParseWireState(response);
        Assert.NotNull(wire);
        var snapshot = NativeMediaCoreStateMapper.MapNativeWireStateToSnapshot(100, 3, wire);
        Assert.Equal(9, snapshot.AppliedControlRevision);
        Assert.Equal(7, snapshot.AudioObservation!.ObservedRevision);
        Assert.Equal("core-a", snapshot.AudioObservation.Epoch);
        Assert.Equal(snapshot.AudioObservation, snapshot.Diagnostics.AudioObservation);
        using var retained = JsonDocument.Parse(CoreObservationModel.Parse(response.RootElement.GetProperty("snapshot").GetRawText()).QualificationJson());
        Assert.Equal(2, retained.RootElement.GetProperty("audioObservation").GetProperty("refused").GetInt32());
    }
    [Theory]
    [InlineData(250, "fresh")]
    [InlineData(251, "aging")]
    [InlineData(1000, "aging")]
    [InlineData(1001, "stale")]
    public void AgeIncludesTimeSinceReceiptWithoutResettingOwnerAge(double elapsed, string expected)
    {
        var now = DateTimeOffset.UtcNow;
        var snapshot = new NativeMediaCoreStateSnapshot
        {
            RawReceivedUtc = now,
            AudioObservation = new() { Epoch = "core-a", Sequence = 3, ObservedRevision = 2, Status = "fresh", AgeMs = 50 },
            AppliedControlRevision = 5
        };
        Assert.Equal(expected, AudioObservationPolicy.State(snapshot, now.AddMilliseconds(elapsed - 50)));
        Assert.Equal(5, snapshot.AppliedControlRevision);
        Assert.Equal(2, snapshot.AudioObservation.ObservedRevision);
    }

    [Fact]
    public void MissingAndUnobservedTelemetryCannotBeHealthy()
    {
        var now = DateTimeOffset.UtcNow;
        Assert.Equal("unobserved", AudioObservationPolicy.State(null, now));
        Assert.Equal("unobserved", AudioObservationPolicy.State(new(), now));
        var snapshot = new NativeMediaCoreStateSnapshot { AudioObservation = new() };
        Assert.Equal("awaiting measured audio", AudioObservationPolicy.MeterWarning(snapshot, now));
    }

    [Fact]
    public void StaleTelemetryKeepsLastMeterAndDoesNotStopOutputs()
    {
        var snapshot = new NativeMediaCoreStateSnapshot
        {
            AudioObservation = new() { Sequence = 4, Status = "stale", AgeMs = 2000 },
            AudioMixSession = new() { Status = "live", Summary = "held", LoudnessLufs = -16.2 }
        };
        Assert.Equal("meters stale — last values held", AudioObservationPolicy.MeterWarning(snapshot));
        Assert.Equal(-16.2, snapshot.AudioMixSession.LoudnessLufs);
        Assert.Equal("live", snapshot.AudioMixSession.Status);
    }
}
