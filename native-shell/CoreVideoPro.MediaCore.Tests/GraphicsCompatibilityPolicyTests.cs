using CoreVideoPro.MediaCore.Models;
using CoreVideoPro.MediaCore.Services;
using System.Text.Json;
using Xunit;
namespace CoreVideoPro.MediaCore.Tests;
public sealed class GraphicsCompatibilityPolicyTests
{
    [Fact] public void ExistingShowControlsOnlyChooseCurrentSceneLowerThirds()
    {
        NativeGraphicsInstance Cue(string id,string scene)=>new() {InstanceId=id,Scope=scene.Length==0?"global":"scene",SceneId=scene,TemplateId="compact-solid"};
        var state=new NativeGraphicsState {Instances=[Cue("global",""),Cue("current","a"),Cue("other","b")]};
        Assert.Equal(new[] {"global","current"},GraphicsCompatibilityPolicy.LowerThirdsForScene(state,"a").Select(i=>i.InstanceId));
    }
    [Fact] public void RebuildPreservesDeclaredFieldsAndBlanksWithoutInjectingUnsupportedTitle()
    {
        var instance=new NativeGraphicsInstance {Fields=new() {Name="Old",Title="Old title",AdditionalFields=new() {["size"]=JsonSerializer.SerializeToElement(42)}}};
        var template=new NativeGraphicsTemplate {Runtime="declarative",Fields=[new() {Id="name",Type="string"},new() {Id="size",Type="number"}]};
        var fields=GraphicsCompatibilityPolicy.SubjectFields(instance,template,"New","")!;
        Assert.Equal("New",fields["name"].GetString());Assert.Equal(42,fields["size"].GetInt32());Assert.False(fields.ContainsKey("title"));
        var compatible=GraphicsCompatibilityPolicy.SubjectFields(instance,null,"New","")!;Assert.Equal("",compatible["title"].GetString());
        Assert.Null(GraphicsCompatibilityPolicy.SubjectFields(instance,template with {Fields=[]},"New",""));
    }
}
