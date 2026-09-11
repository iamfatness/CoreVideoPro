using CoreVideoPro.MediaCore.Models;

namespace CoreVideoPro.MediaCore.Services;

/// <summary>
/// THE SOURCE SET: which Zoom participants are sources of the show, in budget order, with the
/// <c>purpose</c> each is subscribed under. It is the one decision both the raw VIDEO and the
/// per-participant AUDIO subscription lists are built from (and the one the mixer is meant to
/// read later, to list only source channels). Pure: no UI, no I/O.
///
/// <para><b>Only sources that are sources (owner rule, #478, 2026-09-11).</b> "Why are you
/// grabbing sources I don't have routed to the multiviewer? … Wasting bandwidth and resources on
/// sources that never hit the multiviewer or ISO is so painfully bad." The hardware-switcher
/// model: nothing is on air unless it is an input. The complete set, in budget order:</para>
/// <list type="number">
/// <item>Program scene routes</item>
/// <item>Program Tiles wall members (+ a Zoom wall background). A Tiles scene serialises an
/// EMPTY route list, so these must be added explicitly.</item>
/// <item>Preview scene routes</item>
/// <item>Preview Tiles wall members (+ background)</item>
/// <item>The multiview wall: in-show Show Input slots, in slot order</item>
/// <item>Participants armed for ISO recording (only while "Program + ISOs" is on)</item>
/// </list>
/// <para>There is NO roster-order fill. The old builder appended every participant in roster
/// order, so early joiners with their camera OFF took the capped video budget ahead of a wall
/// guest who joined late (live: Alexander, slot 2, froze whenever he was not in
/// Preview/Program), and every participant held an audio subscription.</para>
///
/// <para><b>Video vs audio.</b> A camera-OFF source is still a source: it keeps its AUDIO
/// subscription (a wall guest with the camera off can still speak) but never spends VIDEO
/// budget, because it produces no frames. It is picked up for video on the next spine sync
/// (every 500 ms) once the camera comes on. Only video is capped.</para>
///
/// <para><b>The active speaker is not a tier.</b> Talking grants nothing. The directed speaker
/// is a source only through a route whose mode is <c>active-speaker</c> (a follow-speaker
/// route), and only while their camera is on (the same rule
/// <c>SceneRoutingService.ResolveRouteParticipant</c> uses to pick who that route shows). It is
/// subscribed at the 720p <c>active-speaker</c> purpose, because the person behind a follow route
/// changes with whoever talks and a 1080p purpose there would re-subscribe on every speaker
/// change. Being the speaker never moves anyone's place in the budget either: a moving order is
/// exactly what used to push the last guest past the cap on each flip.</para>
///
/// <para><b>Purpose is a STABLE tier, not a position.</b> A participant holding a FIXED Program
/// route gets <c>program</c>, else a FIXED Preview route <c>preview</c> (both 1080p in the core,
/// <c>native/src/modules/ZoomSubscriptionResolutionPolicy.h</c>); anything else takes the
/// purpose of the first tier that lists it (all 720p). So purpose, and with it resolution,
/// moves only on a Take or a cue, never on who is talking.</para>
/// </summary>
public static class ZoomSourceSetPolicy
{
    public const string ProgramPurpose = "program";
    public const string PreviewPurpose = "preview";
    public const string ProgramTilesPurpose = "program-tiles";
    public const string PreviewTilesPurpose = "preview-tiles";
    public const string MultiviewPurpose = "multiview";
    public const string IsoPurpose = "iso";
    public const string FollowSpeakerPurpose = "active-speaker";

    private const string ZoomSourcePrefix = "zoom:";
    private const string FollowSpeakerRouteMode = "active-speaker";

    public sealed record ParticipantState(string Id, bool VideoOn, bool IsDirectedSpeaker);

    public sealed record Input
    {
        /// <summary>The participants in the selected room, in roster order.</summary>
        public IReadOnlyList<ParticipantState> Participants { get; init; } = [];
        public IReadOnlyList<MediaCoreSceneRouteWire> ProgramRoutes { get; init; } = [];
        public MediaCoreTilesLayerWire? ProgramTiles { get; init; }
        public IReadOnlyList<MediaCoreSceneRouteWire> PreviewRoutes { get; init; } = [];
        public MediaCoreTilesLayerWire? PreviewTiles { get; init; }
        /// <summary>The in-show multiview wall sources (any kind; only Zoom ones count).</summary>
        public IReadOnlyList<MediaCoreMultiviewSourceWire> WallSources { get; init; } = [];
        /// <summary>Bare Zoom participant ids armed for ISO recording.</summary>
        public IReadOnlyList<string> IsoParticipantIds { get; init; } = [];
    }

    public sealed record Source(string ParticipantId, string Purpose, bool VideoOn);

    /// <param name="Subscribed">Camera-on sources within the budget, in budget order.</param>
    /// <param name="OverBudget">Camera-on sources the budget left out. Never silent: the
    /// builder turns these into a warning and a label on the multiview tile.</param>
    public sealed record VideoDecision(IReadOnlyList<Source> Subscribed, IReadOnlyList<Source> OverBudget);

    /// <summary>Every source of the show, in budget order, camera on or off.</summary>
    public static IReadOnlyList<Source> Resolve(Input input)
    {
        var videoOn = new Dictionary<string, bool>(StringComparer.Ordinal);
        string? directedSpeaker = null;
        foreach (var participant in input.Participants)
        {
            if (string.IsNullOrWhiteSpace(participant.Id) || videoOn.ContainsKey(participant.Id))
            {
                continue;
            }

            videoOn[participant.Id] = participant.VideoOn;
            if (participant.IsDirectedSpeaker && directedSpeaker is null)
            {
                directedSpeaker = participant.Id;
            }
        }

        var fixedProgram = FixedRouteParticipants(input.ProgramRoutes);
        var fixedPreview = FixedRouteParticipants(input.PreviewRoutes);

        var sources = new List<Source>();
        var seen = new HashSet<string>(StringComparer.Ordinal);

        void Add(string? participantId, string tierPurpose)
        {
            if (string.IsNullOrWhiteSpace(participantId) ||
                !videoOn.TryGetValue(participantId, out var on) ||
                !seen.Add(participantId))
            {
                return;
            }

            var purpose = fixedProgram.Contains(participantId) ? ProgramPurpose
                : fixedPreview.Contains(participantId) ? PreviewPurpose
                : tierPurpose;
            sources.Add(new Source(participantId, purpose, on));
        }

        void AddRoutes(IReadOnlyList<MediaCoreSceneRouteWire> routes, string fixedPurpose)
        {
            foreach (var route in routes)
            {
                if (IsFollowSpeakerRoute(route))
                {
                    // A follow route shows the speaker only while their camera is on.
                    if (directedSpeaker is not null && videoOn[directedSpeaker])
                    {
                        Add(directedSpeaker, FollowSpeakerPurpose);
                    }
                }
                else
                {
                    Add(BareZoomId(route.ParticipantId), fixedPurpose);
                }
            }
        }

        void AddTiles(MediaCoreTilesLayerWire? tiles, string purpose)
        {
            if (tiles is null)
            {
                return;
            }

            foreach (var member in tiles.Members)
            {
                Add(ZoomMemberId(member), purpose);
            }

            // The wall's live background feed is on air on this bus too.
            Add(ZoomMemberId(tiles.BackgroundSourceId), purpose);
        }

        AddRoutes(input.ProgramRoutes, ProgramPurpose);
        AddTiles(input.ProgramTiles, ProgramTilesPurpose);
        AddRoutes(input.PreviewRoutes, PreviewPurpose);
        AddTiles(input.PreviewTiles, PreviewTilesPurpose);
        foreach (var source in input.WallSources
                     .Select((source, index) => (source, index))
                     .OrderBy(entry => entry.source.Slot)
                     .ThenBy(entry => entry.index)
                     .Select(entry => entry.source))
        {
            if (string.Equals(source.Kind, "zoom", StringComparison.Ordinal))
            {
                Add(BareZoomId(source.ParticipantId), MultiviewPurpose);
            }
        }

        foreach (var participantId in input.IsoParticipantIds)
        {
            Add(BareZoomId(participantId), IsoPurpose);
        }

        return sources;
    }

    /// <summary>Video: camera-on sources only, in source order, capped.</summary>
    public static VideoDecision DecideVideo(IReadOnlyList<Source> sources, int maxVideoSubscriptions)
    {
        var cameraOn = sources.Where(source => source.VideoOn).ToList();
        var budget = Math.Clamp(maxVideoSubscriptions, 0, cameraOn.Count);
        return new VideoDecision(cameraOn.Take(budget).ToList(), cameraOn.Skip(budget).ToList());
    }

    /// <summary>Audio: every source, camera on or off, uncapped.</summary>
    public static IReadOnlyList<string> AudioParticipantIds(IReadOnlyList<Source> sources) =>
        sources.Select(source => source.ParticipantId).ToList();

    private static bool IsFollowSpeakerRoute(MediaCoreSceneRouteWire route) =>
        string.IsNullOrWhiteSpace(route.ParticipantId) &&
        string.IsNullOrWhiteSpace(route.CaptureDeviceId) &&
        string.IsNullOrWhiteSpace(route.MediaAssetId) &&
        string.Equals(route.Mode, FollowSpeakerRouteMode, StringComparison.Ordinal);

    private static HashSet<string> FixedRouteParticipants(IReadOnlyList<MediaCoreSceneRouteWire> routes) =>
        routes
            .Where(route => !IsFollowSpeakerRoute(route))
            .Select(route => BareZoomId(route.ParticipantId))
            .OfType<string>()
            .ToHashSet(StringComparer.Ordinal);

    /// <summary>A route/wall/ISO id: bare Zoom id, or "zoom:"-qualified. Other schemes
    /// (capture:, media:) are not Zoom participants.</summary>
    private static string? BareZoomId(string? id)
    {
        if (string.IsNullOrWhiteSpace(id))
        {
            return null;
        }

        if (id.StartsWith(ZoomSourcePrefix, StringComparison.Ordinal))
        {
            return id[ZoomSourcePrefix.Length..];
        }

        return id.Contains(':') ? null : id;
    }

    /// <summary>A Tiles member is always scheme-qualified (TilesLayerPayloadBuilder.QualifySourceId).</summary>
    private static string? ZoomMemberId(string? member) =>
        member is { Length: > 0 } && member.StartsWith(ZoomSourcePrefix, StringComparison.Ordinal)
            ? member[ZoomSourcePrefix.Length..]
            : null;
}
