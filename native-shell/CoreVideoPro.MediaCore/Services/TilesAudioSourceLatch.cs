using CoreVideoPro.MediaCore.Models;

namespace CoreVideoPro.MediaCore.Services;

/// <summary>
/// TILES AUDIO IS STICKY (#478, controller rulings R3 and N5). The Tiles membership policy
/// (<c>TilesLayerPayloadBuilder</c>) drops a guest the moment their camera goes off, and under
/// "sources only" (<see cref="ZoomSourceSetPolicy"/>) a non-member is not a source — so a panelist
/// on a gallery who turns their camera off to fix a light would lose their AUDIO subscription and
/// go silent on air mid-sentence, then come back cold when the camera returns.
///
/// This latch remembers, PER SCENE, every Zoom participant who has been a Tiles member of that
/// scene while it has been on a bus. They stay an audio source for as long as that scene is on
/// EITHER bus. Keying by scene, not by bus, is what carries them across a Take: a camera-off
/// panelist latched while their gallery sat in Preview stays audible when the Take swaps it onto
/// Program (keyed by bus, both buses changed scene on that Take and both latches cleared — the
/// panelist went silent exactly as their gallery went to air).
///
/// It forgets a scene as soon as it is on neither bus, forgets a participant who leaves the
/// meeting, and forgets EVERYTHING when the meeting session ends (<see cref="Observe"/> with
/// <c>inMeeting: false</c>, and <see cref="Clear"/> on Engine off). That last rule matters because
/// Zoom reuses per-meeting user ids: a latch that outlived the meeting would make a DIFFERENT
/// person of the next meeting (possibly a never-on-camera comms line) audible. It never adds
/// anyone who was not a member: a never-on-camera participant is never eligible, so never latched.
///
/// Stateful by nature, so it is a small owned object rather than part of the pure policy; the
/// spine sync calls <see cref="Observe"/> once per payload. Thread-safe: the spine builder may run
/// off the UI thread.
/// </summary>
public sealed class TilesAudioSourceLatch
{
    private readonly object _gate = new();
    // Insertion-ordered per scene, scenes in first-seen order: the result order is stable.
    private readonly List<(string SceneId, List<string> Members)> _scenes = [];

    /// <summary>
    /// Records this payload's Tiles members for the scenes on both buses and returns every
    /// latched member (bare Zoom ids) of a scene still on a bus who is still present. Outside a
    /// meeting it clears everything and returns nothing.
    /// </summary>
    public IReadOnlyList<string> Observe(
        bool inMeeting,
        string? programSceneId,
        MediaCoreTilesLayerWire? programTiles,
        string? previewSceneId,
        MediaCoreTilesLayerWire? previewTiles,
        IReadOnlyCollection<string> presentParticipantIds)
    {
        lock (_gate)
        {
            if (!inMeeting)
            {
                _scenes.Clear();
                return [];
            }

            var present = presentParticipantIds as IReadOnlySet<string>
                ?? new HashSet<string>(presentParticipantIds, StringComparer.Ordinal);
            var onAir = new Dictionary<string, MediaCoreTilesLayerWire>(StringComparer.Ordinal);
            if (!string.IsNullOrEmpty(programSceneId) && programTiles is not null)
            {
                onAir[programSceneId] = programTiles;
            }

            if (!string.IsNullOrEmpty(previewSceneId) && previewTiles is not null)
            {
                onAir.TryAdd(previewSceneId, previewTiles);
            }

            // A scene on neither bus (or no longer a Tiles scene) is forgotten.
            _scenes.RemoveAll(entry => !onAir.ContainsKey(entry.SceneId));
            foreach (var (sceneId, tiles) in onAir)
            {
                var index = _scenes.FindIndex(entry => string.Equals(entry.SceneId, sceneId, StringComparison.Ordinal));
                if (index < 0)
                {
                    _scenes.Add((sceneId, []));
                    index = _scenes.Count - 1;
                }

                var members = _scenes[index].Members;
                members.RemoveAll(id => !present.Contains(id));  // leaving the meeting ends it
                foreach (var member in tiles.Members)
                {
                    if (ZoomSourceSetPolicy.ZoomMemberId(member) is { } id &&
                        present.Contains(id) &&
                        !members.Contains(id, StringComparer.Ordinal))
                    {
                        members.Add(id);
                    }
                }
            }

            var result = new List<string>();
            var seen = new HashSet<string>(StringComparer.Ordinal);
            foreach (var (_, members) in _scenes)
            {
                foreach (var id in members)
                {
                    if (seen.Add(id))
                    {
                        result.Add(id);
                    }
                }
            }

            return result;
        }
    }

    /// <summary>Forget everything: the meeting session ended (Engine off, leave).</summary>
    public void Clear()
    {
        lock (_gate)
        {
            _scenes.Clear();
        }
    }
}
