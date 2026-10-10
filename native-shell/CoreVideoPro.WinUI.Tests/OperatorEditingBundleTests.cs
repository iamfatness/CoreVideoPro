using CoreVideoPro.MediaCore.Models;
using CoreVideoPro.WinUI.Models;
using CoreVideoPro.WinUI.Services;
using CoreVideoPro.WinUI.ViewModels;
using Xunit;
namespace CoreVideoPro.WinUI.Tests;
public sealed class OperatorEditingBundleTests
{
    [Fact] public void RoiDoesNotChangeGradeOrPresetAndRejectsInvalidGeometry()
    {
        var e=new ColorGradeEditorViewModel("p1","Guest",new() { Lut="none" }); e.AdvancedExpanded=true;
        var original=e.CopyGradeJson();var applied=0;e.GradeChanged+=(_,_)=>applied++;
        e.SetScopeRoi(new(true,.2,.3,.1,.2));var revision=e.Revision;
        Assert.Equal(original,e.CopyGradeJson());Assert.Equal(0,applied);Assert.Equal(1,e.ScopeRoi.Revision);
        Assert.Equal((384,324,193,216),e.ScopeRoi.Pixels(1920,1080));
        e.SetScopeRoi(new(true,.9,0,.2,1));Assert.Equal(revision,e.Revision);
        e.SetScopeRoi(new(true,double.NaN,0,.2,1));Assert.Equal(revision,e.Revision);
        e.ClearScopeRoi();Assert.False(e.ScopeRoi.Enabled);Assert.Equal((0,0,1920,1080),e.ScopeRoi.Pixels(1920,1080));
    }
    [Fact] public void TinyEdgeRoiAlwaysContainsOnePixel()
    {
        Assert.Equal((63,63,1,1),new GradeScopeRoi(true,.999,.999,.001,.001).Pixels(64,64));
    }
    [Fact] public void ContinuousAdjustmentIsOneUndoStepAndRetainsExactValues()
    {
        var e=new ColorGradeEditorViewModel("p1","Guest",new() { Lut="none" });e.AdvancedExpanded=true;e.AddPrimariesCommand.Execute(null);
        var before=e.Document;e.BeginAdjustmentGesture();
        foreach (var value in new[] { .01,.1,.123456789 }) e.EditAdjustment(e.SelectedAdjustment! with { ExposureStops=value });
        e.EndAdjustmentGesture();Assert.Equal(.123456789,e.SelectedAdjustment!.ExposureStops);
        e.UndoGradeCommand.Execute(null);Assert.Equal(before.Operations[0].ExposureStops,e.SelectedAdjustment!.ExposureStops);
        e.RedoGradeCommand.Execute(null);Assert.Equal(.123456789,e.SelectedAdjustment!.ExposureStops);
    }
    [Theory][InlineData("two-up",2)][InlineData("six-up",6)][InlineData("speaker-slides",2)]
    public void StarterScenesHaveDistinctInputsAndAnExplicitShare(string layout,int count)
    {
        var scene=new Scene {Id="test",Layout=layout};var routes=SceneRoutingService.GetStarterRoutes(scene);
        Assert.Equal(count,routes.Count);Assert.DoesNotContain(routes,r=>r.Mode==SourceRouteMode.ActiveSpeaker);
        if (layout=="speaker-slides") Assert.Equal(SourceRouteMode.ScreenShare,routes[^1].Mode);
        var slots=routes.Where(r=>r.ShowInputSlotNumber.HasValue).Select(r=>r.ShowInputSlotNumber).ToArray();
        Assert.Equal(slots.Length,slots.Distinct().Count());
        Assert.Equal(slots,SceneRoutingService.GetRouteDefaults(scene,routes,[]).Where(r=>r.ShowInputSlotNumber.HasValue).Select(r=>r.ShowInputSlotNumber));
    }
    [Fact] public void AppearanceAndNamedLooksPersistWithoutChangingText()
    {
        var look=LowerThirdAppearance.FromPreset("broadcast") with { NameSize=53.25,NameColor="#CAFE12",ShowLogo=false };
        var input=new ProductionOutputPreferences { LowerThirdAppearance=look,LowerThirdPresets=new() { ["Office"]=look } };
        var result=ProductionOutputPreferencesSerializer.Deserialize(ProductionOutputPreferencesSerializer.Serialize(input))!;
        Assert.Equal(look,result.LowerThirdAppearance);Assert.Equal(look,result.LowerThirdPresets["Office"]);
        Assert.True(look.IsValid);Assert.False((look with { Padding=double.PositiveInfinity }).IsValid);
        Assert.False((look with { NameColor="#ZZZZZZ" }).IsValid);
    }
}
