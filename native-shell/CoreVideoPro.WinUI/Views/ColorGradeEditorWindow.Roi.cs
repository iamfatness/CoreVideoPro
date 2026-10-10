using Microsoft.UI.Xaml;
namespace CoreVideoPro.WinUI.Views;
public sealed partial class ColorGradeEditorWindow
{
    private bool _loadingRoi;
    private void RefreshRoi()
    {
        var r=ViewModel.ScopeRoi; var t=ViewModel.NativeSurface.PendingSharedHandle;
        RoiGuide.Visibility=ViewModel.AdvancedExpanded && ViewModel.ScopesEnabled && r.Enabled && ViewModel.ScopeRoiGuideVisible ? Visibility.Visible : Visibility.Collapsed;
        RoiGuide.Update(r,t?.Width??0,t?.Height??0);
        _loadingRoi=true;
        try { RoiX.Maximum=1-r.Width; RoiY.Maximum=1-r.Height; RoiWidth.Maximum=1-r.X; RoiHeight.Maximum=1-r.Y;
            RoiX.Value=r.X; RoiY.Value=r.Y; RoiWidth.Value=r.Width; RoiHeight.Value=r.Height; }
        finally { _loadingRoi=false; }
    }
    private void OnRoiNumberChanged(object? sender,double value)
    {
        if (_loadingRoi) return;
        ViewModel.SetScopeRoi(ViewModel.ScopeRoi with { X=RoiX.Value,Y=RoiY.Value,Width=RoiWidth.Value,Height=RoiHeight.Value });
    }
    private void OnClearRoi(object sender,RoutedEventArgs e) => ViewModel.ClearScopeRoi();
    private void OnResetRoi(object sender,RoutedEventArgs e) => ViewModel.ResetScopeRoi();
}
