using CoreVideoPro.MediaCore.Models;
using CoreVideoPro.MediaCore.Services;
using System.Text.Json;
using Xunit;
namespace CoreVideoPro.MediaCore.Tests;
public sealed class GraphicsCompatibilityPolicyTests
{
    [Fact] public async Task ExplicitLegacyAutomaticIntentUsesNativeArmingAndHideWithoutEnablingSceneTake()
    {
        var state=new NativeGraphicsState {SchemaVersion=1,CoreEpoch="core",Automation=new() {Configuration=new() {Rules=[new() {Id="intro",InstanceId="guest"}]}}};
        var actions=new List<string>();
        var session=new GraphicsWorkspaceSession((_,body,_)=>{
            var input=JsonDocument.Parse(body).RootElement;var action=input.GetProperty("action").GetString()!;actions.Add(action);
            state=state with {Automation=action switch {"enable-automatic"=>state.Automation! with {Mode="automatic",Armed=true},_=>state.Automation! with {Armed=false}}};
            return Task.FromResult(new NativeGraphicsOperationResult {Accepted=true});
        },()=>state);session.Observe(state);
        Assert.True((await GraphicsCompatibilityPolicy.SetAutomationEnabledAsync(session,true)).Accepted);Assert.True(GraphicsCompatibilityPolicy.AutomationEnabled(state));
        Assert.Equal(new[] {"enable-automatic"},actions);
        Assert.True((await GraphicsCompatibilityPolicy.SetAutomationEnabledAsync(session,false)).Accepted);Assert.False(GraphicsCompatibilityPolicy.AutomationEnabled(state));Assert.Equal("hide-automatic",actions[^1]);
    }
    [Fact] public async Task MissingRulesAndManualHoldsCannotBeMistakenForEnabledAutomation()
    {
        var state=new NativeGraphicsState {SchemaVersion=1,CoreEpoch="core",Automation=new()};var sends=0;
        var session=new GraphicsWorkspaceSession((_,_,_)=>{sends++;throw new InvalidOperationException("No command should be sent");},()=>state);session.Observe(state);
        Assert.False((await GraphicsCompatibilityPolicy.SetAutomationEnabledAsync(session,true)).Accepted);
        state=state with {Automation=new() {Mode="automatic",Armed=true,HeldInstances=["guest"],Configuration=new() {Rules=[new() {Id="intro",InstanceId="guest"}]}}};session.Observe(state);
        Assert.False(GraphicsCompatibilityPolicy.AutomationEnabled(state));Assert.False((await GraphicsCompatibilityPolicy.SetAutomationEnabledAsync(session,true)).Accepted);Assert.Equal(0,sends);
    }
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
