using CoreVideoPro.MediaCore.Models;

namespace CoreVideoPro.MediaCore.Services;

/// <summary>
/// THE SOURCE SET: which Zoom participants are sources of the show, in budget order, with the
/// <c>purpose</c> each is subscribed under. It is the one decision the raw VIDEO list, the
/// per-participant AUDIO list and the core's speaker director are all built from (and the one
/// the mixer is meant to read later, to list only source channels). Pure: no UI, no I/O.
///
/// <para><b>Only sources that are sources (owner rule, #478, 2026-09-11).</b> "Why are you
/// grabbing sources I don't have routed to the multiviewer? … Wasting bandwidth and resources on
/// sources that never hit the multiviewer or ISO is so painfully bad." The hardware-switcher
/// model: nothing is on air unless it is an input. The complete set, in budget order:</para>
/// <list type="number">
/// <item>Program scene routes</item>
/// <item>Preview scene routes</item>
/// <item>Program Tiles wall members (+ a Zoom wall background). A Tiles scene serialises an
/// EMPTY route list, so these must be added explicitly.</item>
/// <item>Preview Tiles wall members (+ background)</item>
/// <item>The multiview wall: in-show Show Input slots, in slot order</item>
/// <item>Participants armed for ISO recording (only while "Program + ISOs" is on)</item>
/// <item>Sticky Tiles audio members (see below): audio only, and only when no tier above
/// already lists them</item>
/// </list>
/// <para>Fixed bus ROUTES come before Tiles members (fix round 1, review finding 8): an eligible
/// Tiles gallery can list every camera-on guest (MaxTiles up to 64), and must never push the
/// operator's explicitly cued Preview guest past the budget. There is NO roster-order fill: the
/// old builder appended every participant, so camera-OFF early joiners took the capped budget
/// ahead of a wall guest who joined late (live: Alexander, slot 2).</para>
///
/// <para><b>Video vs audio.</b> A camera-OFF source is still a source: it keeps its AUDIO
/// subscription (a wall guest with the camera off can still speak) but never spends VIDEO
/// budget; it is picked up for video on the next spine sync (every 500 ms) once the camera
/// comes on. Only video is capped. <b>Tiles audio is sticky (controller ruling R3):</b> a Tiles
/// member of a scene on a bus stays an audio source while that scene stays on that bus, even
/// after the membership policy drops them for turning their camera off — otherwise a panelist
/// fixing a light goes silent on air mid-sentence. The latch is
/// <see cref="TilesAudioSourceLatch"/>; a never-on-camera participant never becomes a member,
/// so never becomes audible this way.</para>
///
/// <para><b>The active speaker is not a tier, and never grants a feed (R1).</b> The core's
/// speaker director follows the talker only AMONG these sources (the payload names them in
/// <c>sourceParticipantIds</c>), so the directed speaker is always already a source. A
/// follow-speaker (<c>active-speaker</c> mode) route therefore adds nobody and moves nobody's
/// budget position; it only gives the directed speaker the bus purpose (<c>program</c> /
/// <c>preview</c>, the 1080p tier) for as long as they are directed.</para>
///
/// <para><b>Purpose is a STABLE tier, not a position.</b> A participant holding a Program route
/// gets <c>program</c>, else a Preview route <c>preview</c> (both 1080p in the core,
/// <c>native/src/modules/ZoomSubscriptionResolutionPolicy.h</c>); anything else takes the
/// purpose of the first tier that lists it (all 720p). So purpose, and with it resolution,
/// moves only on a Take or a cue — or, for a follow-speaker route only, on a change of directed
/// speaker.</para>
/// </summary>
public static class ZoomSourceSetPolicy
{
    public const string ProgramPurpose = "program";
    public const string PreviewPurpose = "preview";
    public const string ProgramTilesPurpose = "program-tiles";
    public const string PreviewTilesPurpose = "preview-tiles";
    public const string TilesAudioPurpose = "tiles-audio";
    public const string MultiviewPurpose = "multiview";
    public const string IsoPurpose = "iso";

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
        /// <summary>Bare Zoom ids latched as Tiles members of a scene still on its bus (R3).</summary>
        public IReadOnlyList<string> StickyAudioParticipantIds { get; init; } = [];
        /// <summary>The in-show multiview wall sources (any kind; only Zoom ones count).</summary>
        public IReadOnlyList<MediaCoreMultiviewSourceWire> WallSources { get; init; } = [];
        /// <summary>Bare Zoom participant ids armed for ISO recording.</summary>
        public IReadOnlyList<string> IsoParticipantIds { get; init; } = [];
    }

    /// <param name="VideoOn">False for a camera-off source AND for a sticky audio-only one.</param>
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
            // The first directed participant WITH video — the same pick
            // SceneRoutingService.ResolveRouteParticipant makes for a follow route.
            if (participant.IsDirectedSpeaker && participant.VideoOn && directedSpeaker is null)
            {
                directedSpeaker = participant.Id;
            }
        }

        var onProgramRoute = RouteParticipants(input.ProgramRoutes, directedSpeaker);
        var onPreviewRoute = RouteParticipants(input.PreviewRoutes, directedSpeaker);

        var sources = new List<Source>();
        var seen = new HashSet<string>(StringComparer.Ordinal);

        void Add(string? participantId, string tierPurpose, bool audioOnly = false)
        {
            if (string.IsNullOrWhiteSpace(participantId) ||
                !videoOn.TryGetValue(participantId, out var on) ||
                !seen.Add(participantId))
            {
                return;
            }

            var purpose = onProgramRoute.Contains(participantId) ? ProgramPurpose
                : onPreviewRoute.Contains(participantId) ? PreviewPurpose
                : tierPurpose;
            sources.Add(new Source(participantId, purpose, on && !audioOnly));
        }

        void AddFixedRoutes(IReadOnlyList<MediaCoreSceneRouteWire> routes, string purpose)
        {
            foreach (var route in routes)
            {
                // A follow-speaker route adds nobody: its directed speaker is already a
                // source (the director picks only among sources) and is listed where their
                // own tier puts them, with this bus's purpose.
                if (!IsFollowSpeakerRoute(route))
                {
                    Add(BareZoomId(route.ParticipantId), purpose);
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

        AddFixedRoutes(input.ProgramRoutes, ProgramPurpose);
        AddFixedRoutes(input.PreviewRoutes, PreviewPurpose);
        AddTiles(input.ProgramTiles, ProgramTilesPurpose);
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

        // LAST, so every real tier above wins: a sticky id that is also on the wall or ISO
        // keeps its video there. Only a latched member with no other tier is audio-only.
        foreach (var participantId in input.StickyAudioParticipantIds)
        {
            Add(BareZoomId(participantId), TilesAudioPurpose, audioOnly: true);
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

    /// <summary>
    /// The ids the core's speaker director may follow (R1). The same set as audio: every source,
    /// camera on or off (the director itself requires video before it directs anyone).
    /// </summary>
    public static IReadOnlyList<string> SpeakerCandidateIds(IReadOnlyList<Source> sources) =>
        sources.Select(source => source.ParticipantId).ToList();

    private static bool IsFollowSpeakerRoute(MediaCoreSceneRouteWire route) =>
        string.IsNullOrWhiteSpace(route.ParticipantId) &&
        string.IsNullOrWhiteSpace(route.CaptureDeviceId) &&
        string.IsNullOrWhiteSpace(route.MediaAssetId) &&
        string.Equals(route.Mode, FollowSpeakerRouteMode, StringComparison.Ordinal);

    /// <summary>Who holds a route on this bus: every fixed route's guest, plus the directed
    /// speaker when the bus carries a follow-speaker route.</summary>
    private static HashSet<string> RouteParticipants(
        IReadOnlyList<MediaCoreSceneRouteWire> routes,
        string? directedSpeaker)
    {
        var ids = new HashSet<string>(StringComparer.Ordinal);
        foreach (var route in routes)
        {
            if (IsFollowSpeakerRoute(route))
            {
                if (directedSpeaker is not null)
                {
                    ids.Add(directedSpeaker);
                }
            }
            else if (BareZoomId(route.ParticipantId) is { } participantId)
            {
                ids.Add(participantId);
            }
        }

        return ids;
    }

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
    internal static string? ZoomMemberId(string? member) =>
        member is { Length: > 0 } && member.StartsWith(ZoomSourcePrefix, StringComparison.Ordinal)
            ? member[ZoomSourcePrefix.Length..]
            : null;
}
