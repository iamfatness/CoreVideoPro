using CoreVideoPro.WinUI.Models;
using CoreVideoPro.WinUI.ViewModels.Transport;
using Xunit;

namespace CoreVideoPro.WinUI.Tests;

public sealed class SceneTakeRulesTests
{
    private static SourceRoute Route(string id = "layer") => new()
    {
        Id = id, Mode = SourceRouteMode.Fixed, ParticipantId = "guest-a",
        CanvasRect = new NormalizedCanvasRect { X = 0, Y = 0, Width = 1, Height = 1 }
    };

    [Fact]
    public void UntouchedDraftIsNotTakeable()
    {
        var program = new[] { Route() };
        Assert.False(SceneTakeRules.HasPendingChanges(program, program.Select(route => route.Clone()).ToList()));
    }

    [Theory]
    [InlineData("geometry")]
    [InlineData("source")]
    [InlineData("framing")]
    [InlineData("opacity")]
    [InlineData("border")]
    public void OrdinaryCanvasChangesAreTakeable(string edit)
    {
        var route = Route();
        var draft = route.Clone();
        switch (edit)
        {
            case "geometry": draft.CanvasRect!.Width = 0.5; break;
            case "source": draft.ParticipantId = "guest-b"; break;
            case "framing": draft.SourceOffsetX = 0.2; break;
            case "opacity": draft.Opacity = 0.5; break;
            case "border": draft.BorderThickness += 2; break;
        }
        Assert.True(SceneTakeRules.HasPendingChanges([route], [draft]));
    }

    [Fact]
    public void LayerOrderAndRemovalAreTakeable()
    {
        var first = Route("first");
        var second = Route("second");
        Assert.True(SceneTakeRules.HasPendingChanges([first, second], [second, first]));
        Assert.True(SceneTakeRules.HasPendingChanges([first, second], [first]));
    }
}
