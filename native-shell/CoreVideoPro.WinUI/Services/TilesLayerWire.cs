using CoreVideoPro.MediaCore.Models;

namespace CoreVideoPro.WinUI.Services;

/// <summary>
/// The flattening of a <see cref="TilesLayerPayload"/> onto the media-core wire
/// record, extracted verbatim from <c>StudioViewModel.BuildTilesLayerWire</c>
/// (behaviour-neutral) so <c>TilesFixtureTests</c> can run the shared
/// <c>contracts/tiles.fixtures.json</c> through the exact production path that the
/// Mac shell's Swift builder is checked against.
/// </summary>
public static class TilesLayerWire
{
    public static MediaCoreTilesLayerWire? From(TilesLayerPayload? payload) =>
        payload is null
            ? null
            : new MediaCoreTilesLayerWire(
                payload.LayerId,
                payload.Order,
                payload.Members,
                payload.Style.TileAspect,
                payload.Style.CustomAspectRatio,
                payload.Style.GutterPercent,
                payload.Style.MarginPercent,
                payload.Style.BackgroundColor,
                payload.Style.BorderShape, payload.Style.BorderColor, payload.Style.BorderThickness,
                payload.Style.CornerRadius, payload.Style.GlowColor, payload.Style.GlowSize,
                payload.Style.GlowIntensity, payload.Style.GlowSoftness,
                payload.Style.AnimateLayout, payload.Style.AnimationDurationMs, payload.Style.FillMode, payload.Style.BackgroundSourceId,
                payload.Overrides?.ToDictionary(p => p.Key, p => new MediaCoreTilesOverrideWire(
                    p.Value.Rect is { } r ? new MediaCoreTilesRectWire(r.X, r.Y, r.Width, r.Height) : null,
                    p.Value.CropLeftPercent, p.Value.CropRightPercent, p.Value.Z)),
                payload.Rect is { } wallRect ? new MediaCoreTilesRectWire(wallRect.X, wallRect.Y, wallRect.Width, wallRect.Height) : null,
                payload.Opacity, payload.TransparentBackground,
                NameEachTile: payload.Style.NameEachTile);
}
