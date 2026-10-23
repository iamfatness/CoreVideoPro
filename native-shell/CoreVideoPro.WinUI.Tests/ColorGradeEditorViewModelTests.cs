using CoreVideoPro.MediaCore.Models;
using CoreVideoPro.WinUI.Models;
using CoreVideoPro.WinUI.ViewModels;
using Xunit;
namespace CoreVideoPro.WinUI.Tests;
public sealed class ColorGradeEditorViewModelTests
{
    private static ColorGradeEditorViewModel Editor() => new("p1", "Camera", new() { Lut = "none" });
    private static GradePreviewObservation Ready(ColorGradeEditorViewModel editor, long? revision = null) => new()
    {
        InstanceId = editor.InstanceId, SourceId = editor.SourceId, Revision = revision ?? editor.Revision,
        Status = "ready", Texture = new() { SharedHandleHex = "0x1234", Width = 1920, Height = 1080 }
    };
    [Fact] public void RejectsOldRevisionWrongSourceAndWrongEditorAndClearsPictureOnEdit()
    {
        var editor = Editor(); editor.ObserveNativePreview(Ready(editor)); Assert.True(editor.HasPreview);
        Assert.True(VideoSurfacePresentationRules.UsesGpuSharedTexture(editor.NativeSurface.SurfaceKey, editor.NativeSurface.Kind));
        Assert.False(editor.NativeSurface.HasPreviewBitmap);
        editor.Exposure = 2; Assert.False(editor.HasPreview);
        editor.ObserveNativePreview(Ready(editor, 0)); Assert.False(editor.HasPreview);
        editor.ObserveNativePreview(Ready(editor) with { SourceId = "p2" }); Assert.False(editor.HasPreview);
        editor.ObserveNativePreview(Ready(editor) with { InstanceId = "other" }); Assert.False(editor.HasPreview);
        editor.ObserveNativePreview(Ready(editor)); Assert.True(editor.HasPreview);
    }
    [Fact] public void DraftAndOriginalComparisonDoNotChangeOnAirGradeButApplyDoes()
    {
        var editor = Editor(); var changes = 0; ColorGrade? saved = null;
        editor.GradeChanged += (_, _) => ++changes; editor.GradeSaved += (_, grade) => saved = grade;
        editor.LiveEditing = false; editor.Exposure = 8; editor.Lut = "warm-film";
        editor.CompareOriginal = true; Assert.Equal(0, changes); Assert.Equal("none", editor.PreviewGrade.Lut);
        Assert.Equal(0, editor.PreviewGrade.Exposure);
        editor.CompareOriginal = false; Assert.Equal("warm-film", editor.PreviewGrade.Lut);
        editor.SaveCommand.Execute(null); Assert.NotNull(saved); Assert.Equal(8, saved!.Exposure);
        Assert.Equal(0, changes); editor.LiveEditing = true; Assert.Equal(1, changes);
        editor.Exposure = 9; Assert.Equal(2, changes);
    }
    [Theory] [InlineData("held")] [InlineData("stale")]
    public void HeldPictureIsLabeledAndUnavailableHasNoFallback(string status)
    {
        var editor = Editor(); editor.ObserveNativePreview(Ready(editor) with { Status = status });
        Assert.True(editor.HasPreview); Assert.Contains("held", editor.PreviewStatus);
        editor.SetNativeUnavailable("Not built"); Assert.False(editor.HasPreview);
        Assert.Equal("Not built", editor.PreviewStatus);
    }
}
