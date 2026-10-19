using System.Text.Json;
using CoreVideoPro.MediaCore.Models;
using CoreVideoPro.MediaCore.Services;
using Xunit;

namespace CoreVideoPro.MediaCore.Tests;

public sealed class GraphicsShowStartersTests
{
    private static NativeGraphicsState Catalog => new() { CoreEpoch = "starter-test", Templates =
        new[] { "minimal-accent", "broadcast", "logo-accent", "compact-solid", "centered-title", "corner-logo", "clock-panel" }
        .Select(id => new NativeGraphicsTemplate { TemplateId = id, TemplateVersion = 1, ManifestHash = "pin-" + id,
            Runtime = id is "compact-solid" or "minimal-accent" or "broadcast" ? "compatibility" : "declarative",
            Family = id == "centered-title" ? "title" : id == "corner-logo" ? "bug" : id == "clock-panel" ? "clock" : "lower-third",
            Fields = new[] { "name", "title", "heading", "subtitle", "label", "time" }.Select(f => new NativeGraphicsTemplateField { Id = f, Type = "string", Default = JsonSerializer.SerializeToElement("") }).ToArray()
        }).ToArray() };

    [Fact] public void EveryShowStartsWithPopulatedCoordinatedDraftsAndPreviewHasNoNativeSideEffects()
    {
        var sends = 0; var state = Catalog;
        var session = new GraphicsWorkspaceSession((_, _, _) => { sends++; return Task.FromResult(new NativeGraphicsOperationResult()); }, () => state);
        session.Observe(state);
        foreach (var starter in GraphicsShowStarters.All) {
            var graphics = GraphicsShowStarters.Prepare(starter, state);
            Assert.Equal(4, graphics.Count);
            foreach (var graphic in graphics) {
                Assert.True(graphic.Draft.Style.IsValid); Assert.Equal(starter.Accent, graphic.Draft.Style.AccentColor);
                session.PreviewStarter(graphic);
                Assert.Null(session.Selected); Assert.Equal("", session.PreviewValidation);
                Assert.Null(session.Draft!.Binding); Assert.Equal("fade", session.Draft.Animations.In.Type);
            }
            Assert.Equal("Alex Rivera", graphics[0].Draft.Fields.GetProperty("name").GetString());
            Assert.Equal(starter.Heading, graphics[1].Draft.Fields.GetProperty("heading").GetString());
        }
        Assert.Equal(0, sends); Assert.Empty(state.Instances);
    }

    [Fact] public async Task AddingAStarterOnlyCreatesHiddenNamedCuesAndPreservesTheExistingOnAirGraphic()
    {
        var existing = new NativeGraphicsInstance { InstanceId = "existing", ProgramRevision = 7, Revision = 9 };
        var state = Catalog with { Instances = [existing] };
        var commands = new List<string>();
        var session = new GraphicsWorkspaceSession((action, json, _) => {
            commands.Add(action); var body = JsonDocument.Parse(json).RootElement; var id = body.GetProperty("instanceId").GetString()!;
            if (action == "graphics.instance.create") state = state with { Instances = [..state.Instances, new() {
                InstanceId = id, Revision = 1, TemplateId = body.GetProperty("templateId").GetString()!, TemplateVersion = 1,
                Fields = JsonSerializer.Deserialize<NativeGraphicsFields>(body.GetProperty("fields"), new JsonSerializerOptions(JsonSerializerDefaults.Web))! }] };
            else state = state with { Instances = state.Instances.Select(i => i.InstanceId == id ? i with { Label = body.GetProperty("label").GetString()! } : i).ToArray() };
            return Task.FromResult(new NativeGraphicsOperationResult { Accepted = true, CurrentRevision = 1, InstanceId = id });
        }, () => state);
        session.Observe(state);
        Assert.True(await session.AddStarterAsync(GraphicsShowStarters.Prepare(GraphicsShowStarters.All[0], state)));
        Assert.Equal(existing, state.Instances[0]); Assert.Equal(5, state.Instances.Count);
        Assert.All(state.Instances.Skip(1), i => { Assert.Equal(0, i.ProgramRevision); Assert.Equal(0, i.CueRevision); Assert.NotEmpty(i.Label); });
        Assert.All(commands, c => Assert.Contains(c, new[] { "graphics.instance.create", "graphics.instance.update" }));
        Assert.Equal("Interview · Guest introduction", session.Selected!.Label);
        Assert.Equal(4,session.LastAddedStarterIds.Count);Assert.DoesNotContain("existing",session.LastAddedStarterIds);
    }

    [Fact] public void BrandingAndLogoReplacementRetainSampleTextAndPlacementWithoutMutatingTheOriginalSet()
    {
        var original=GraphicsShowStarters.Prepare(GraphicsShowStarters.All[0],Catalog);
        var branded=GraphicsShowStarters.WithBranding(original,"My weekly show","#2873AA");
        Assert.Equal("My weekly show",branded[1].Draft.Fields.GetProperty("heading").GetString());
        Assert.Equal("Alex Rivera",branded[0].Draft.Fields.GetProperty("name").GetString());
        Assert.Equal("In conversation",original[1].Draft.Fields.GetProperty("heading").GetString());
        var recolored=GraphicsShowStarters.WithAccentColor(branded,"#C17844");
        Assert.Equal("My weekly show",recolored[1].Draft.Fields.GetProperty("heading").GetString());
        Assert.Equal("Alex Rivera",recolored[0].Draft.Fields.GetProperty("name").GetString());
        var image=new NativeGraphicsImageReference("image-test",1,"logo");
        var imported=new NativeGraphicsTemplate {TemplateId=image.TemplateId,TemplateVersion=1,Family="bug",Runtime="declarative"};
        var result=GraphicsShowStarters.WithLogo(branded,imported,image);
        Assert.Equal(image,result[0].Draft.Image);Assert.Equal("image-test",result[2].Draft.TemplateId);
        Assert.Equal(original[2].Draft.Placement,result[2].Draft.Placement);Assert.Null(original[0].Draft.Image);
        Assert.Throws<InvalidOperationException>(()=>GraphicsShowStarters.WithBranding(original,"Title","red"));
        Assert.Throws<InvalidOperationException>(()=>GraphicsShowStarters.WithLogo(original,imported with {TemplateVersion=2},image));
    }

    [Fact] public async Task CapacityAndChangedTemplatePinsRefuseBeforeCreatingAnything()
    {
        var state = Catalog; var graphics = GraphicsShowStarters.Prepare(GraphicsShowStarters.All[0], state); var sends = 0;
        var session = new GraphicsWorkspaceSession((_, _, _) => { sends++; return Task.FromResult(new NativeGraphicsOperationResult()); }, () => state);
        state = state with { Instances = Enumerable.Range(1, 5).Select(n => new NativeGraphicsInstance { InstanceId = "kept-" + n }).ToArray() };
        session.Observe(state); Assert.False(await session.AddStarterAsync(graphics)); Assert.Contains("four free", session.Status);
        state = Catalog with { Templates = Catalog.Templates.Select(t => t with { ManifestHash = "replaced" }).ToArray() };
        session.Observe(state); Assert.False(await session.AddStarterAsync(graphics)); Assert.Equal(0, sends);
    }

    [Fact] public async Task PartialAdmissionIsReportedAndNeverShowsOrEvictsAnything()
    {
        var state = Catalog; var actions = new List<string>();
        var session = new GraphicsWorkspaceSession((action, _, _) => { actions.Add(action); return Task.FromResult(new NativeGraphicsOperationResult { Accepted = false, Reason = "graphics-resource-budget" }); }, () => state);
        session.Observe(state);
        Assert.False(await session.AddStarterAsync(GraphicsShowStarters.Prepare(GraphicsShowStarters.All[0], state)));
        Assert.Equal(new[] { "graphics.instance.create" }, actions); Assert.Contains("Nothing was shown", session.Status);
    }
}
