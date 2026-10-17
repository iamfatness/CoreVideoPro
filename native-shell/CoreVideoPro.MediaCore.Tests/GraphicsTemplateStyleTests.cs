using System.Text.Json;
using CoreVideoPro.MediaCore.Models;
using CoreVideoPro.MediaCore.Services;
using Xunit;

namespace CoreVideoPro.MediaCore.Tests;
public sealed class GraphicsTemplateStyleTests
{
    private static readonly NativeGraphicsTemplate Template=new() { TemplateId="interview",Runtime="declarative",TemplateVersion=1,
        Fields=[new(){Id="copy",Type="string"},new(){Id="size",Type="number",Minimum=10,Maximum=100},new(){Id="enabled",Type="boolean"}],
        Layers=JsonDocument.Parse("[{\"type\":\"text\",\"text\":{\"field\":\"copy\"}}]").RootElement.Clone() };
    [Fact]
    public void StylePreservesCopyAndBlanksWhileRestoringNativeStyleFields()
    {
        var draft=new NativeGraphicsDraft("interview",1,JsonSerializer.SerializeToElement(new {copy="Original",size=42,enabled=true}),new(),new());
        var style=GraphicsTemplateStyle.Capture(draft,Template);
        Assert.False(style.Fields.TryGetProperty("copy",out _));
        var edited=draft with {Fields=JsonSerializer.SerializeToElement(new {copy="",size=20,enabled=false})};
        Assert.True(GraphicsTemplateStyle.TryApply(edited,Template,style,out var restored));
        Assert.Equal("",restored.Fields.GetProperty("copy").GetString());
        Assert.Equal(42,restored.Fields.GetProperty("size").GetInt32());
        Assert.True(restored.Fields.GetProperty("enabled").GetBoolean());
        Assert.False(GraphicsTemplateStyle.TryApply(edited,Template,style with {TemplateId="another"},out _));
        Assert.False(GraphicsTemplateStyle.TryApply(edited,Template,style with {Fields=JsonSerializer.SerializeToElement(new {size=500})},out _));
        Assert.False(GraphicsTemplateStyle.TryApply(edited,Template,style with {Fields=JsonSerializer.SerializeToElement(new {copy="wrong subject"})},out _));
    }
}
