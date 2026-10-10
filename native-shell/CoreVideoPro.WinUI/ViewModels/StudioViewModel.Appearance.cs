using CoreVideoPro.MediaCore.Models;
namespace CoreVideoPro.WinUI.ViewModels;
public sealed partial class StudioViewModel
{
    internal Services.GradePreviewCoordinator CreateAppearancePreview(ColorGradeEditorViewModel editor) => new(_bridge,editor,RunOnUiThread);
    internal void RestoreLowerThirdAppearance(LowerThirdAppearance appearance) => BrandKit=BrandKit.WithAppearance(appearance);
    internal void ApplyLowerThirdAppearance(LowerThirdAppearance appearance) {
        BrandKit=BrandKit.WithAppearance(appearance);Overlays.NotifyBrandKitChanged();SaveProductionOutputPreferences();_ = TrySyncMediaCoreAsync();
    }
    internal void SaveAppearancePreferences() => SaveProductionOutputPreferences();
}
