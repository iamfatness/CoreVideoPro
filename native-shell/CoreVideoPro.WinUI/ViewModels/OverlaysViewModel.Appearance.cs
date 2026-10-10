using CoreVideoPro.MediaCore.Models;
namespace CoreVideoPro.WinUI.ViewModels;
public sealed partial class OverlaysViewModel
{
    public Dictionary<string,LowerThirdAppearance> LowerThirdPresets { get; } = new(StringComparer.OrdinalIgnoreCase);
    public LowerThirdAppearance AppliedAppearance => BrandKit.LowerThirdAppearance ?? new();
    public string AppliedLookLabel => BrandKit.LowerThirdAppearance?.Preset ?? "Existing look (unchanged)";
    public string AppliedLookAnchor => BrandKit.LowerThirdAppearance?.Anchor ?? LowerThirdPosition;
    public bool ApplyAppearance(LowerThirdAppearance appearance)
    {
        if (!appearance.IsValid) return false;
        _studio.ApplyLowerThirdAppearance(appearance);
        OnPropertyChanged(nameof(AppliedAppearance)); return true;
    }
    public void RestoreAppearance(LowerThirdAppearance? appearance,Dictionary<string,LowerThirdAppearance>? presets)
    {
        LowerThirdPresets.Clear();
        foreach (var p in (presets ?? []).Take(32))
            if (!string.IsNullOrWhiteSpace(p.Key) && p.Key.Length<=80 && p.Value?.IsValid==true) LowerThirdPresets[p.Key]=p.Value;
        if (appearance?.IsValid == true) _studio.RestoreLowerThirdAppearance(appearance);
    }
    public bool SaveAppearancePreset(string name,LowerThirdAppearance appearance)
    {
        name=name.Trim(); if (name.Length is <1 or >80 || !appearance.IsValid || (LowerThirdPresets.Count>=32 && !LowerThirdPresets.ContainsKey(name))) return false;
        LowerThirdPresets[name]=appearance; _studio.SaveAppearancePreferences(); return true;
    }
}
