using CoreVideoPro.WinUI.Models;
using CoreVideoPro.WinUI.ViewModels;
using Xunit;

namespace CoreVideoPro.WinUI.Tests;

public sealed class ColorGradeEditorViewModelTests
{
    [Fact]
    public void BuildsGradedPreviewFromSourceSurface()
    {
        var surface = VideoSurfaceState
            .Waiting(VideoSurfaceKind.Multiview, "participant:p1", "Camera 1")
            .WithPreviewPixels(
                [
                    10, 20, 30, 255,
                    40, 50, 60, 255
                ],
                2,
                1);

        var editor = new ColorGradeEditorViewModel(
            "p1",
            "Camera 1",
            new ColorGrade
            {
                Lut = "none",
                Exposure = 0,
                Contrast = 0,
                Saturation = 0,
                Temperature = 0
            },
            surface);

        Assert.True(editor.HasPreview);
        Assert.Equal(2, editor.GradedPreviewWidth);
        Assert.Equal(1, editor.GradedPreviewHeight);
        Assert.Equal(surface.PreviewBgra, editor.GradedPreviewBgra);
    }

    [Fact]
    public void RebuildsPreviewAndRaisesLiveChangeWhenControlsMove()
    {
        var surface = VideoSurfaceState
            .Waiting(VideoSurfaceKind.Multiview, "participant:p1", "Camera 1")
            .WithPreviewPixels(
                [
                    80, 90, 100, 255,
                    110, 120, 130, 255
                ],
                2,
                1);

        var editor = new ColorGradeEditorViewModel(
            "p1",
            "Camera 1",
            new ColorGrade
            {
                Lut = "none",
                Exposure = 0,
                Contrast = 0,
                Saturation = 0,
                Temperature = 0
            },
            surface);
        var before = editor.GradedPreviewBgra!.ToArray();
        ColorGrade? liveGrade = null;
        editor.GradeChanged += (_, grade) => liveGrade = grade;

        editor.Exposure = 25;

        Assert.NotNull(liveGrade);
        Assert.Equal(25, liveGrade!.Exposure);
        Assert.NotEqual(before, editor.GradedPreviewBgra);
    }
}
