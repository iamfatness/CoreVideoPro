using CoreVideoPro.MediaCore.Models;
using CoreVideoPro.WinUI.Models;
using CoreVideoPro.WinUI.Services;
using CoreVideoPro.WinUI.ViewModels;
using Xunit;
namespace CoreVideoPro.WinUI.Tests;
public sealed class AdvancedGradeEditorTests
{
    private static ColorGradeEditorViewModel Editor() => new("p1","Guest",new() { Lut="warm-film",Exposure=2 });
    private static GradePreviewObservation Ready(ColorGradeEditorViewModel e,long applied=0,ulong epoch=7) => new() {
        InstanceId=e.InstanceId,SourceId=e.SourceId,Revision=e.Revision,AppliedRevision=applied,SourceEpoch=epoch,SourceFrameId=11,
        Status="ready",Texture=new() { SharedHandleHex="0x1234",Width=1920,Height=1080,Format="B8G8R8A8_UNORM" } };
    [Fact] public void ExpansionIsDraftAndStackCurvesSurviveCollapseUndoAndPresets()
    {
        var editor=Editor();var changes=0;editor.GradeChanged+=(_,_)=>++changes;
        editor.AdvancedExpanded=true;Assert.False(editor.LiveEditing);
        editor.AddCurvesCommand.Execute(null);editor.SetCurve(1,[new(0,0),new(.333,.7),new(1,1)]);
        Assert.Equal(.7,editor.CurrentGrade.Advanced!.Operations[0].Curves[1][1].Y);
        Assert.Equal(0,changes);editor.AdvancedExpanded=false;Assert.True(editor.HasAdvancedAdjustments);
        editor.UndoGradeCommand.Execute(null);Assert.Equal(2,editor.Document.Operations[0].Curves[1].Length);
        editor.RedoGradeCommand.Execute(null);Assert.Equal(3,editor.Document.Operations[0].Curves[1].Length);
        var preset=editor.CopyGradeJson();var loaded=Editor();loaded.LiveEditing=false;Assert.True(loaded.PasteGradeJson(preset));
        Assert.Equal("warm-film",loaded.Lut);Assert.Equal(2,loaded.Exposure);Assert.Equal(.7,loaded.Document.Operations[0].Curves[1][1].Y);
        var prefs=new ProductionOutputPreferences { SourceGrades=new() { ["zoom-person:stable"] = editor.CurrentGrade } };
        var decoded=ProductionOutputPreferencesSerializer.Deserialize(ProductionOutputPreferencesSerializer.Serialize(prefs));
        Assert.NotNull(decoded);Assert.Equal(.333,decoded!.SourceGrades["zoom-person:stable"].Advanced!.Operations[0].Curves[1][1].X);
    }
    [Fact] public void UndoRestoresEntirePresetAndBasicChangesAsOneAction()
    {
        var editor=Editor();editor.LiveEditing=false;editor.Exposure=9;
        editor.UndoGradeCommand.Execute(null);Assert.Equal(2,editor.Exposure);
        editor.RedoGradeCommand.Execute(null);Assert.Equal(9,editor.Exposure);
        var preset=new ColorGradeEditorViewModel("p2","Other",new() { Lut="punch",Exposure=15 });
        preset.LiveEditing=false;preset.AddCurvesCommand.Execute(null);
        Assert.True(editor.PasteGradeJson(preset.CopyGradeJson()));Assert.Equal(15,editor.Exposure);Assert.True(editor.HasAdvancedAdjustments);
        editor.UndoGradeCommand.Execute(null);Assert.Equal(9,editor.Exposure);Assert.Equal("warm-film",editor.Lut);Assert.False(editor.HasAdvancedAdjustments);
        editor.RedoGradeCommand.Execute(null);Assert.Equal(15,editor.Exposure);Assert.True(editor.HasAdvancedAdjustments);
    }
    [Fact] public async Task ApplyWaitsForAckAndConflictsRetainDraftWithoutSaving()
    {
        var editor=new ColorGradeEditorViewModel("p1","Guest",new() { Lut="warm-film",Exposure=2 },4);editor.AdvancedExpanded=true;editor.ObserveNativePreview(Ready(editor,4));
        var ack=new TaskCompletionSource<SourceGradeApplyOutcome>();long expected=-1;var saved=0;
        editor.ApplyGradeAsync=(_,epoch,revision)=> {Assert.Equal(7ul,epoch);expected=revision;return ack.Task;};
        editor.GradeSaved+=(_,_)=>++saved;
        var pending=editor.SaveCommand.ExecuteAsync(null);Assert.True(editor.IsApplying);Assert.Equal(0,saved);Assert.Equal(4,expected);
        ack.SetResult(new(false,5,7,"grade-revision-conflict"));await pending;Assert.False(editor.IsApplying);Assert.Equal(0,saved);
        Assert.Contains("conflict",editor.EditStatus);
    }
    [Fact] public async Task AcknowledgedApplyUpdatesExpectedRevisionButAnotherEditorsObservationCannot()
    {
        var editor=Editor();editor.AdvancedExpanded=true;editor.ObserveNativePreview(Ready(editor,0));var revisions=new List<long>();
        editor.ApplyGradeAsync=(_,epoch,revision)=> {revisions.Add(revision);return Task.FromResult(new SourceGradeApplyOutcome(true,revision+1,epoch,""));};
        await editor.ApplyLiveCommand.ExecuteAsync(null);editor.ObserveNativePreview(Ready(editor,5));await editor.ApplyLiveCommand.ExecuteAsync(null);
        Assert.Equal(new long[] {0,1},revisions);
        editor.ObserveNativePreview(Ready(editor,5,8));Assert.False(editor.ApplyLiveCommand.CanExecute(null));
    }
    [Fact] public async Task FirstObservationCannotSilentlyRebaseAnAlreadyOpenedDraft()
    {
        var editor=Editor();editor.AdvancedExpanded=true;editor.ObserveNativePreview(Ready(editor,2));
        long expected=-1;editor.ApplyGradeAsync=(_,epoch,revision)=> { expected=revision;return Task.FromResult(new SourceGradeApplyOutcome(false,2,epoch,"grade-revision-conflict")); };
        await editor.ApplyLiveCommand.ExecuteAsync(null);Assert.Equal(0,expected);Assert.Equal(0,editor.AppliedRevision);Assert.Contains("conflict",editor.EditStatus);
    }
    [Fact] public async Task RecoveryDuringPendingApplyCannotSaveAnObsoleteAcknowledgement()
    {
        var editor=Editor();editor.AdvancedExpanded=true;editor.ObserveNativePreview(Ready(editor));
        var ack=new TaskCompletionSource<SourceGradeApplyOutcome>();var saved=0;
        editor.ApplyGradeAsync=(_,_,_)=>ack.Task;editor.GradeSaved+=(_,_)=>++saved;
        var pending=editor.SaveCommand.ExecuteAsync(null);Assert.False(editor.CanEditControls);
        editor.ResetGradeAuthority();ack.SetResult(new(true,1,7,""));await pending;
        Assert.Equal(0,saved);Assert.True(editor.CanEditControls);Assert.Contains("Source changed",editor.EditStatus);
    }
    [Fact] public void UnsupportedAndMalformedDocumentsAreRejectedBeforeEditing()
    {
        var editor=Editor();var before=editor.Revision;
        Assert.False(editor.PasteGradeJson("{\"version\":3,\"operations\":[]}"));Assert.Equal(before,editor.Revision);
        Assert.False(editor.PasteGradeJson("{\"version\":2,\"operations\":null}"));Assert.Equal(before,editor.Revision);
        Assert.False(editor.PasteGradeJson("{\"version\":2,\"operations\":[null]}"));Assert.Equal(before,editor.Revision);
    }
}
