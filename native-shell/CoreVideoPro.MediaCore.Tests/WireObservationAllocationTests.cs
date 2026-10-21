using System.Text.Json;
using CoreVideoPro.MediaCore.Services;
using Xunit;

namespace CoreVideoPro.MediaCore.Tests;

public sealed class WireObservationAllocationTests
{
    [Fact]
    public void TypedWireProjectionDoesNotCopyDiagnosticPayloadItOmits()
    {
        using var response = JsonDocument.Parse(JsonSerializer.Serialize(new { ok = true, state = new {
            health = new { status = "ready" }, profile = (object?)null, sceneId = "actual",
            realtimeEvidence = new { samples = new string('x', 300_000) }
        } }));
        Assert.NotNull(CoreProtocolParser.TryParseWireState(response));
        var before = GC.GetAllocatedBytesForCurrentThread();
        for (var i = 0; i < 20; i++)
            Assert.Equal("actual", CoreProtocolParser.TryParseWireState(response)!.SceneId);
        Assert.InRange(GC.GetAllocatedBytesForCurrentThread() - before, 0, 1_000_000);
        Assert.Equal(300_000, response.RootElement.GetProperty("state").GetProperty("realtimeEvidence")
            .GetProperty("samples").GetString()!.Length); // The raw observation is still intact.
    }
}
