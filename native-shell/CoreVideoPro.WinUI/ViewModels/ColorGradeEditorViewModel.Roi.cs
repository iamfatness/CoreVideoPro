using CoreVideoPro.MediaCore.Models;

namespace CoreVideoPro.WinUI.ViewModels;

public sealed partial class ColorGradeEditorViewModel
{
    private LowerThirdPreview? _lowerThirdPreview;
    public LowerThirdPreview? LowerThirdPreview => _lowerThirdPreview;
    internal void SetLowerThirdPreview(LowerThirdPreview preview) { _lowerThirdPreview=preview; RequestPreview(); }
    private GradeScopeRoi _scopeRoi = new();
    public GradeScopeRoi ScopeRoi => _scopeRoi;
    public string ScopeRegionLabel => _scopeRoi.Enabled ? $"ROI · {_scopeRoi.X:P1}, {_scopeRoi.Y:P1} · {_scopeRoi.Width:P1} × {_scopeRoi.Height:P1} · selection {_scopeRoi.Revision}" : "Full frame";
    public bool ScopeRoiEnabled {
        get => _scopeRoi.Enabled;
        set => SetScopeRoi(value && !_scopeRoi.Enabled && _scopeRoi.Width==1 && _scopeRoi.Height==1
            ? new(true,.25,.25,.5,.5) : _scopeRoi with { Enabled = value });
    }
    private bool _scopeRoiGuideVisible=true;
    public bool ScopeRoiGuideVisible { get=>_scopeRoiGuideVisible;set { if (SetProperty(ref _scopeRoiGuideVisible,value)) OnPropertyChanged(nameof(ScopeRoiGuideVisible)); } }
    public void ResetScopeRoi() => SetScopeRoi(new(true,.25,.25,.5,.5));
    public void SetScopeRoi(GradeScopeRoi roi)
    {
        if (!roi.IsValid || roi with { Revision = 0 } == _scopeRoi with { Revision = 0 }) return;
        _scopeRoi = roi with { Revision = _scopeRoi.Revision + 1 };
        OnPropertyChanged(nameof(ScopeRoi)); OnPropertyChanged(nameof(ScopeRoiEnabled));
        OnPropertyChanged(nameof(ScopeRegionLabel));ScopeStatus="Updating scope region; waiting for matching native measurement.";
        RequestPreview();
    }
    public void ClearScopeRoi() => SetScopeRoi(new());
}
