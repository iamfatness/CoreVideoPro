using Microsoft.UI.Xaml;
using CoreVideoPro.MediaCore.Models;
namespace CoreVideoPro.WinUI.Views;
public sealed partial class ColorGradeEditorWindow
{
    private bool _loadingRoi;
    private void RefreshRoi()
    {
        var r=ViewModel.ScopeRoi; var t=ViewModel.NativeSurface.PendingSharedHandle;
        if(r.Shape=="circle" && t is {Width:>0,Height:>0} && Math.Abs(r.Width*t.Width-r.Height*t.Height)>.01) {
            ViewModel.SetScopeRoi(ScopeRoiGeometry.CircleSize(r,r.Width*t.Width,t.Width,t.Height));
            r=ViewModel.ScopeRoi;
        }
        RoiGuide.Visibility=RoiGuide.DrawingShape is not null || (r.Enabled && ViewModel.ScopeRoiGuideVisible) ? Visibility.Visible : Visibility.Collapsed;
        // A new measurement briefly clears the shared handle. Keep the fitted
        // image geometry so consecutive pointer moves can finish the gesture.
        RoiGuide.Update(r,t is {Width:>0,Height:>0}?t.Width:RoiGuide.SourceWidth,
            t is {Width:>0,Height:>0}?t.Height:RoiGuide.SourceHeight);
        RectangleRoiTool.IsChecked=RoiGuide.DrawingShape=="rectangle";
        CircleRoiTool.IsChecked=RoiGuide.DrawingShape=="circle";
        SelectRoiTool.IsChecked=RoiGuide.DrawingShape is null;
        RoiToolHint.Text=RoiGuide.DrawingShape is { } shape ? $"Drag to draw a {shape} over the subject. Escape cancels." :
            r.Enabled ? "Drag inside to move; drag a handle to resize. Scopes measure the selection; grading affects the whole source." :
            "Choose a shape, then drag over the subject to measure its scopes.";
        _loadingRoi=true;
        try { RoiX.Maximum=1-r.Width; RoiY.Maximum=1-r.Height; RoiWidth.Maximum=1-r.X; RoiHeight.Maximum=1-r.Y;
            RoiX.Value=r.X; RoiY.Value=r.Y; RoiWidth.Value=r.Width; RoiHeight.Value=r.Height; }
        finally { _loadingRoi=false; }
    }
    private void OnRoiNumberChanged(object? sender,double value)
    {
        if (_loadingRoi) return;
        var roi=ViewModel.ScopeRoi with { X=RoiX.Value,Y=RoiY.Value,Width=RoiWidth.Value,Height=RoiHeight.Value };
        if(roi.Shape=="circle" && RoiGuide.SourceWidth>0 && RoiGuide.SourceHeight>0) {
            var diameter=ReferenceEquals(sender,RoiHeight) ? RoiHeight.Value*RoiGuide.SourceHeight : RoiWidth.Value*RoiGuide.SourceWidth;
            roi=ScopeRoiGeometry.CircleSize(roi,diameter,RoiGuide.SourceWidth,RoiGuide.SourceHeight);
        }
        ViewModel.SetScopeRoi(roi);
    }
    internal void ArmRoi(string shape) {
        ViewModel.AdvancedExpanded=true; ViewModel.ScopesEnabled=true; ViewModel.ScopeRoiGuideVisible=true;
        RoiGuide.SetDrawingTool(shape); RefreshRoi();
        RoiGuide.Focus(FocusState.Programmatic);
    }
    private void OnDrawRectangleRoi(object sender,RoutedEventArgs e) => ArmRoi("rectangle");
    private void OnDrawCircleRoi(object sender,RoutedEventArgs e) => ArmRoi("circle");
    private void OnSelectRoi(object sender,RoutedEventArgs e) {RoiGuide.SetDrawingTool(null);ViewModel.ScopeRoiGuideVisible=true;RefreshRoi();}
    private void OnClearRoi(object sender,RoutedEventArgs e) {RoiGuide.SetDrawingTool(null);ViewModel.ClearScopeRoi();RefreshRoi();}
    private void OnResetRoi(object sender,RoutedEventArgs e) => ViewModel.ResetScopeRoi();
}
