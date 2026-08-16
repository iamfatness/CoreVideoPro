using CoreVideoPro.WinUI.Models;

namespace CoreVideoPro.WinUI.Services;

/// <summary>
/// T1 core wall: builds the "tiles" wire payload the shell sends for a CoreVideo
/// Tiles (dynamic-gallery) scene. The core now solves the grid layout itself
/// every frame (compositor::expandTilesLayer / MediaCore.cpp parseTilesLayer) —
/// the shell's only remaining job is MEMBERSHIP POLICY: which participants are
/// ELIGIBLE for a tile (roster, video-off filter, max-tiles cap), in roster
/// order. Whether an eligible member actually has fresh frames to show is the
/// CORE's decision (it is the process receiving them, and it already implements
/// a staleness veto) — this builder must NOT filter on liveness/health beyond
/// video-off, or the two ends drift.
///
/// Extracted from <c>StudioViewModel.ReconcileDynamicGalleryRoutes</c> (which
/// used to ALSO solve rects and emit N scene routes — that job moved into the
/// core; a gallery scene now contributes no scene routes at all, only this
/// payload) so the policy is independently testable without the god object,
/// mirroring <c>IsoSourceSelectionResolver</c> / <c>NativeUvcCapturePolicy</c>.
/// </summary>
public static class TilesLayerPayloadBuilder
{
    public static TilesLayerPayload? Build(
        Scene scene,
        IReadOnlyList<Participant> roomVideoParticipants)
    {
        if (scene.DynamicGallery is not { } settings)
        {
            return null;
        }

        // MEMBERSHIP POLICY ONLY. Whether a member actually has frames is the core's
        // decision (compositor::admitTilesMembers) — it is the process receiving them.
        // Deciding it twice is how the two ends drift.
        var members = roomVideoParticipants
            .Where(participant => participant.Health != FeedHealth.VideoOff)
            .Select(participant => participant.Id)
            .Distinct(StringComparer.Ordinal)
            .Take(Math.Clamp(settings.MaxTiles, 1, 64))
            .ToList();

        return new TilesLayerPayload(
            LayerId: $"tiles:{scene.Id}",
            Order: 0,
            Members: members,
            Style: new TilesStylePayload(
                TileAspect: DynamicGalleryLayoutService.NormalizeAspectPreset(settings.TileAspect),
                CustomAspectRatio: settings.CustomAspectRatio,
                GutterPercent: settings.GutterPercent,
                MarginPercent: settings.MarginPercent,
                BackgroundColor: "#000000"));
    }
}

public sealed record TilesLayerPayload(
    string LayerId,
    int Order,
    IReadOnlyList<string> Members,
    TilesStylePayload Style);

public sealed record TilesStylePayload(
    string TileAspect,
    double CustomAspectRatio,
    double GutterPercent,
    double MarginPercent,
    string BackgroundColor);
