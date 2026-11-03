using System.Text.Json;
using CoreVideoPro.MediaCore.Models;
using CoreVideoPro.MediaCore.Services;
using Xunit;

namespace CoreVideoPro.MediaCore.Tests;

/// <summary>Spec §4 "where values come from stays on the playlist row … or typed": the Operate row
/// inspector offers a box for every extra text variable of the row's template.</summary>
public sealed class GraphicsRowFieldsTests
{
    private const string DesignId = "design-lower-third-a1b2c3";
    private static NativeGraphicsTemplateField Text(string id, int? max = null) => new() { Id = id, Type = "string", Default = JsonSerializer.SerializeToElement(""), MaxLength = max };
    private static NativeGraphicsState Installed(params NativeGraphicsTemplateField[] fields) => new() {
        Templates = [new() { TemplateId = DesignId, TemplateVersion = 2, Runtime = "declarative", Fields = fields }] };
    private static GraphicsPlaylistRow GuestRow => new() { Id = "guest", Kind = GraphicsPlaylistRow.Guest, Name = "Guest", FollowSources = true, TemplateId = DesignId, TemplateVersion = 2 };

    [Fact]
    public void ADesignRowWithAnExtraTextVariableOffersItForTyping()
    {
        var colour = new NativeGraphicsTemplateField { Id = "tint", Type = "color", Default = JsonSerializer.SerializeToElement("#FFFFFF") };
        var choice = Text("side") with { Choices = [JsonSerializer.SerializeToElement("left"), JsonSerializer.SerializeToElement("right")] };
        var state = Installed(Text("name", 128), Text("title", 256), Text("location", 40), Text("time"), Text("label"), colour, choice, Text("venue-hall"));
        var typeable = GraphicsRowFields.Typeable(GuestRow, state, []);
        Assert.Equal(["location", "venue-hall"], typeable.Select(f => f.Id));
        Assert.Equal("Location", typeable[0].Label); Assert.Equal(40, typeable[0].MaxLength);
        Assert.Equal("Venue hall", typeable[1].Label); Assert.Null(typeable[1].MaxLength);
    }

    [Fact]
    public void TheVariablesFriendlyLabelWinsAndTheRowsTitleLineIsNotRepeated()
    {
        var labelled = Text("location", 40) with { ExtensionData = new() { ["metadata"] = JsonSerializer.SerializeToElement(new { label = "Where" }) } };
        var state = Installed(Text("heading"), Text("subtitle"), labelled);
        var row = GuestRow with { Kind = GraphicsPlaylistRow.Title, FollowSources = false };
        var typeable = GraphicsRowFields.Typeable(row, state, [], titleKey: "subtitle");
        Assert.Equal(["heading", "location"], typeable.Select(f => f.Id));
        Assert.Equal("Where", typeable[1].Label);
    }

    [Fact]
    public void WithoutAnInstalledTemplateTheSavedPinDeclaresTheVariables()
    {
        var manifest = GraphicsDesignSeeds.Manifest("lower-third", DesignId, 2, "Vars");
        var scratch = GraphicsTemplateEditor.AddField(NativeGraphicsScratch.From(JsonSerializer.SerializeToElement(manifest)), "location", "text", 40);
        var pin = new GraphicsSavedTemplate(DesignId, 2, scratch.Hash, JsonDocument.Parse(scratch.Manifest).RootElement.Clone());
        Assert.Equal(["location"], GraphicsRowFields.Typeable(GuestRow, new NativeGraphicsState(), [pin]).Select(f => f.Id));
        // A compatibility template has only name/title; a row with no template has nothing to type.
        var compat = new NativeGraphicsState { Templates = [new() { TemplateId = "compact-solid", TemplateVersion = 1, Runtime = "compatibility", Fields = [Text("name"), Text("title"), Text("extra")] }] };
        Assert.Empty(GraphicsRowFields.Typeable(GuestRow with { TemplateId = "compact-solid", TemplateVersion = 1 }, compat, []));
        Assert.Empty(GraphicsRowFields.Typeable(GuestRow with { TemplateId = "", TemplateVersion = 0 }, compat, []));
    }

    [Fact]
    public void TypedTextIsStoredOnTheRowAndReadBack()
    {
        var row = GraphicsRowFields.WithText(GuestRow, "location", "Oslo");
        Assert.Equal("Oslo", GraphicsRowFields.Text(row, "location"));
        Assert.Equal("", GraphicsRowFields.Text(GuestRow, "location"));
        Assert.Equal("Bergen", GraphicsRowFields.Text(GraphicsRowFields.WithText(row, "location", "Bergen"), "location"));
    }
}
