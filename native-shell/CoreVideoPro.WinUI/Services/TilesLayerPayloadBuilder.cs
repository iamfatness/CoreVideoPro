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
        var eligible = roomVideoParticipants
            .Where(participant => participant.Health != FeedHealth.VideoOff)
            .Select(participant => QualifySourceId(participant.Id))
            .Distinct(StringComparer.Ordinal)
            .ToList();
        var members = TilesMembershipPolicy.Resolve(settings, eligible);

        return new TilesLayerPayload(
            LayerId: $"tiles:{scene.Id}",
            Order: 0,
            Members: members,
            Style: new TilesStylePayload(
                TileAspect: DynamicGalleryLayoutService.NormalizeAspectPreset(settings.TileAspect),
                CustomAspectRatio: settings.CustomAspectRatio,
                GutterPercent: settings.GutterPercent,
                MarginPercent: settings.MarginPercent,
                BackgroundColor: SceneRoutingService.NormalizeBorderColor(settings.BackgroundColor),
                BorderShape: settings.BorderShape == "rounded" ? "rounded" : "square",
                BorderColor: SceneRoutingService.NormalizeBorderColor(settings.BorderColor),
                BorderThickness: FiniteClamp(settings.BorderThickness, 0, 32),
                CornerRadius: FiniteClamp(settings.CornerRadius, 0, 100, 16),
                GlowColor: SceneRoutingService.NormalizeBorderColor(settings.GlowColor),
                GlowSize: FiniteClamp(settings.GlowSize, 0, 64),
                GlowIntensity: FiniteClamp(settings.GlowIntensity, 0, 100, 100),
                GlowSoftness: FiniteClamp(settings.GlowSoftness, 0, 100),
                AnimateLayout: settings.AnimateLayout,
                AnimationDurationMs: Math.Clamp(settings.AnimationDurationMs, 100, 2000), FillMode: settings.AutoFill ? "auto" : "manual",
                BackgroundSourceId: settings.BackgroundSourceId),
            Overrides: settings.Overrides.ToDictionary(p => p.Key, p => p.Value.Clone(), StringComparer.Ordinal));
    }

    /// <summary>
    /// Fix (coordinator review, 2026-08-15): <c>StudioViewModel.RoomVideoParticipants</c>
    /// carries BARE Zoom SDK ids (<c>LiveProductionSync.MapRawParticipants</c> sets
    /// <c>Id = participant.UserId.Trim()</c> — no scheme prefix — and
    /// <c>ParticipantMapper.ToParticipant</c> carries that straight through), but the wire
    /// contract's `members` entries must be scheme-qualified
    /// (<c>"zoom:&lt;pid&gt;" | "capture:&lt;id&gt;"</c>): the core's frame-age matcher
    /// (<c>MediaCore.cpp</c> ~line 4864) only pairs a Zoom frame's bare participantId against a
    /// member spelled EXACTLY <c>"zoom:"+pid</c> — an unqualified member never matches, so it's
    /// silently never admitted and the wall renders with no guests on it.
    ///
    /// Mirrors <c>MediaCore::normalizeIsoSourceId</c> (MediaCore.cpp:1936-1941): a member that
    /// already carries a scheme (contains ':') passes through unchanged — do NOT blindly
    /// prepend, or a <c>capture:&lt;id&gt;</c> member becomes <c>zoom:capture:&lt;id&gt;</c> and
    /// matches nothing either. Delegates the actual "zoom:" prefix to
    /// <see cref="ShowInputRosterService.ZoomSourceId"/> — the same helper
    /// <c>BuildMultiviewLayoutSources</c> uses for the analogous <c>SourceId</c> field — rather
    /// than writing a third copy of the scheme string.
    /// </summary>
    private static string QualifySourceId(string id) =>
        id.Contains(':') ? id : ShowInputRosterService.ZoomSourceId(id);

    private static double FiniteClamp(double value, double min, double max, double fallback = 0) =>
        double.IsFinite(value) ? Math.Clamp(value, min, max) : fallback;
}

public sealed record TilesLayerPayload(
    string LayerId,
    int Order,
    IReadOnlyList<string> Members,
    TilesStylePayload Style, IReadOnlyDictionary<string, TilesMemberOverride>? Overrides = null);

public sealed record TilesStylePayload(
    string TileAspect,
    double CustomAspectRatio,
    double GutterPercent,
    double MarginPercent,
    string BackgroundColor,
    string BorderShape = "square", string BorderColor = "#000000", double BorderThickness = 0,
    double CornerRadius = 16, string GlowColor = "#FFFFFF", double GlowSize = 0,
    double GlowIntensity = 100, double GlowSoftness = 0, bool AnimateLayout = false, int AnimationDurationMs = 350, string FillMode = "auto", string BackgroundSourceId = "");
