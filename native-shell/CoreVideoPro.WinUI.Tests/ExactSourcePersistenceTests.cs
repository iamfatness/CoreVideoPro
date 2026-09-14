using System.Text.Json;
using CoreVideoPro.MediaCore.Models;
using CoreVideoPro.WinUI.Models;
using CoreVideoPro.WinUI.Services;
using Xunit;

namespace CoreVideoPro.WinUI.Tests;

public sealed class ExactSourcePersistenceTests
{
    [Theory]
    [InlineData("camera")]
    [InlineData("share")]
    public void CloneAndPersistedJsonRoundTripRetainExactPin(string kind)
    {
        var reference = new ExactSourceReference("source", "instance", "epoch", 7, kind);
        var route = new SourceRoute { Id = "route", Mode = SourceRouteMode.Fixed, ExactSource = reference };
        var clone = route.Clone();
        Assert.Equal(reference, clone.ExactSource);
        route.ExactSource = new ExactSourceReference("source", "new-instance", "epoch", 8, kind);
        Assert.Equal(reference, clone.ExactSource);
        var persisted = ScenePersistenceService.ToPersisted(clone);
        var restoredDto = JsonSerializer.Deserialize<PersistedSceneRoute>(JsonSerializer.Serialize(persisted))!;
        var restored = ScenePersistenceService.FromPersisted(restoredDto);
        Assert.Equal(reference, restored.ExactSource);
        Assert.Null(restored.ParticipantId);
    }

    [Fact]
    public void LegacyRouteDoesNotInventExactIdentity()
    {
        var persisted = JsonSerializer.Deserialize<PersistedSceneRoute>("{\"Id\":\"r\",\"Mode\":\"fixed\",\"ParticipantId\":\"42\"}")!;
        Assert.Null(ScenePersistenceService.FromPersisted(persisted).ExactSource);
    }
}
