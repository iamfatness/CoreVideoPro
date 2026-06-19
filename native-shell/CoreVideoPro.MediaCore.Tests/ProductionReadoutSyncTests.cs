using CoreVideoPro.MediaCore.Models;
using CoreVideoPro.MediaCore.Services;
using Xunit;

namespace CoreVideoPro.MediaCore.Tests;

public sealed class ProductionReadoutSyncTests
{
    [Fact]
    public void ResolveColorGradePatchPreservesLocalStateWhenEngineOff()
    {
        var snapshot = BuildSnapshotWithGrade(exposure: 2, contrast: -1);

        Assert.Null(ProductionReadoutSync.ResolveColorGradePatch(snapshot, engineRunning: false));
    }

    [Fact]
    public void ResolveColorGradePatchReadsRenderPlanWhenEngineOn()
    {
        var snapshot = BuildSnapshotWithGrade(exposure: 2, contrast: -1, saturation: 4, temperature: -3);

        var patch = ProductionReadoutSync.ResolveColorGradePatch(snapshot, engineRunning: true);

        Assert.NotNull(patch);
        Assert.Equal("broadcast", patch!.Lut);
        Assert.Equal(2, patch.Exposure);
        Assert.Equal(-1, patch.Contrast);
        Assert.Equal(4, patch.Saturation);
        Assert.Equal(-3, patch.Temperature);
    }

    [Fact]
    public void ResolveBrandKitPatchPreservesLocalStateWhenEngineOff()
    {
        var snapshot = BuildSnapshotWithBrandKit();

        Assert.Null(ProductionReadoutSync.ResolveBrandKitPatch(snapshot, engineRunning: false));
    }

    [Fact]
    public void ResolveBrandKitPatchReadsSnapshotWhenEngineOn()
    {
        var snapshot = BuildSnapshotWithBrandKit();

        var patch = ProductionReadoutSync.ResolveBrandKitPatch(snapshot, engineRunning: true);

        Assert.NotNull(patch);
        Assert.Equal("Acme Live", patch!.Name);
        Assert.Equal("ACME", patch.LogoText);
        Assert.Equal("logo-1", patch.LogoAssetId);
        Assert.Equal("Acme Logo", patch.LogoAssetName);
        Assert.Equal(@"C:\media\logo.png", patch.LogoAssetPath);
        Assert.Equal("#112233", patch.BrandColor);
        Assert.Equal("Poppins", patch.FontFamily);
        Assert.Equal("large uppercase captions", patch.CaptionStyle);
        Assert.Equal("bug-and-live", patch.DefaultOverlayBehavior);
    }

    private static NativeMediaCoreStateSnapshot BuildSnapshotWithGrade(
        double exposure,
        double contrast,
        double saturation = 0,
        double temperature = 0) =>
        new()
        {
            RenderPlan = new NativeMediaCoreRenderPlan
            {
                RenderPlanId = "test-plan",
                ColorGrade = new NativeMediaCoreColorGrade
                {
                    Lut = "broadcast",
                    Exposure = exposure,
                    Contrast = contrast,
                    Saturation = saturation,
                    Temperature = temperature
                },
                Layers = [],
                Routes = []
            }
        };

    private static NativeMediaCoreStateSnapshot BuildSnapshotWithBrandKit() =>
        new()
        {
            BrandKit = new NativeMediaCoreBrandKit
            {
                Name = "Acme Live",
                LogoText = "ACME",
                LogoAssetId = "logo-1",
                LogoAssetName = "Acme Logo",
                LogoAssetPath = @"C:\media\logo.png",
                BrandColor = "#112233",
                AccentColor = "#445566",
                BackgroundColor = "#000000",
                FontFamily = "Poppins",
                LowerThirdStyle = "solid",
                CaptionStyle = "large uppercase captions",
                DefaultOverlayBehavior = "bug-and-live",
                Summary = "Acme kit live"
            }
        };
}
