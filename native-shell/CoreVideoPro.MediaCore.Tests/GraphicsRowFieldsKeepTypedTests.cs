using System.Text.Json;
using CoreVideoPro.MediaCore.Models;
using CoreVideoPro.MediaCore.Services;
using Xunit;

namespace CoreVideoPro.MediaCore.Tests;

/// <summary>Final re-review I4: a Designer re-pin keeps only text the operator can type on the Operate row.</summary>
public sealed class GraphicsRowFieldsKeepTypedTests
{
    private static GraphicsPlaylistRow Row(params (string Key, string Value)[] fields) => new()
    {
        Id = "row-1", Kind = GraphicsPlaylistRow.Guest, Name = "Guest",
        Fields = fields.ToDictionary(f => f.Key, f => JsonSerializer.SerializeToElement(f.Value), StringComparer.Ordinal),
    };

    [Fact]
    public void TypedExtraVariablesAndTheTitleLineSurviveARepin()
    {
        var kept = GraphicsRowFields.KeepTyped(Row(("location", ""), ("title", "Sample"), ("name", "Sample name")), Row(("location", "Oslo"), ("title", "Host")));
        Assert.Equal("Oslo", GraphicsRowFields.Text(kept, "location"));
        Assert.Equal("Host", GraphicsRowFields.Text(kept, "title"));
    }

    [Fact]
    public void VariablesWithNoOperateBoxFollowTheDesigner()
    {
        var kept = GraphicsRowFields.KeepTyped(Row(("name", "New sample"), ("time", "10:00:00"), ("label", "LIVE")), Row(("name", "Old sample"), ("time", "12:00:00"), ("label", "OLD")));
        Assert.Equal("New sample", GraphicsRowFields.Text(kept, "name"));
        Assert.Equal("10:00:00", GraphicsRowFields.Text(kept, "time"));
        Assert.Equal("LIVE", GraphicsRowFields.Text(kept, "label"));
    }
}
