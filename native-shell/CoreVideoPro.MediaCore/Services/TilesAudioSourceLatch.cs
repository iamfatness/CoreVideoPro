using CoreVideoPro.MediaCore.Models;

namespace CoreVideoPro.MediaCore.Services;

/// <summary>
/// TILES AUDIO IS STICKY (#478, controller ruling R3). The Tiles membership policy
/// (<c>TilesLayerPayloadBuilder</c>) drops a guest the moment their camera goes off, and under
/// "sources only" (<see cref="ZoomSourceSetPolicy"/>) a non-member is not a source — so a panelist
/// on the Program gallery who turns their camera off to fix a light would lose their AUDIO
/// subscription and go silent on air mid-sentence, then come back cold when the camera returns.
///
/// This latch remembers, per bus, every Zoom participant who has been a Tiles member of the
/// scene CURRENTLY on that bus. They stay an audio source until that bus's scene changes (a Take
/// or a new cue) or they leave the meeting. It never adds anyone who was not a member: a
/// never-on-camera participant (a comms line) is never eligible, so never becomes audible.
///
/// Stateful by nature, so it is a small owned object rather than part of the pure policy; the
/// spine sync calls <see cref="Observe"/> once per payload. Thread-safe: the spine builder may run
/// off the UI thread.
/// </summary>
public sealed class TilesAudioSourceLatch
{
    private readonly object _gate = new();
    private readonly BusLatch _program = new();
    private readonly BusLatch _preview = new();

    /// <summary>
    /// Records this payload's Tiles members for both buses and returns every latched member
    /// (bare Zoom ids) who is still in <paramref name="presentParticipantIds"/>.
    /// </summary>
    public IReadOnlyList<string> Observe(
        string? programSceneId,
        MediaCoreTilesLayerWire? programTiles,
        string? previewSceneId,
        MediaCoreTilesLayerWire? previewTiles,
        IReadOnlyCollection<string> presentParticipantIds)
    {
        var present = presentParticipantIds as IReadOnlySet<string>
            ?? new HashSet<string>(presentParticipantIds, StringComparer.Ordinal);
        lock (_gate)
        {
            _program.Observe(programSceneId, programTiles, present);
            _preview.Observe(previewSceneId, previewTiles, present);
            var result = new List<string>();
            var seen = new HashSet<string>(StringComparer.Ordinal);
            foreach (var id in _program.Members.Concat(_preview.Members))
            {
                if (seen.Add(id))
                {
                    result.Add(id);
                }
            }

            return result;
        }
    }

    private sealed class BusLatch
    {
        private string? _sceneId;
        private bool _hasTiles;
        public List<string> Members { get; } = [];

        public void Observe(string? sceneId, MediaCoreTilesLayerWire? tiles, IReadOnlySet<string> present)
        {
            // A different scene on this bus, or the bus is no longer a Tiles scene: start over.
            if (!string.Equals(sceneId, _sceneId, StringComparison.Ordinal) || tiles is null || !_hasTiles ||
                !string.Equals(tiles.LayerId, _layerId, StringComparison.Ordinal))
            {
                Members.Clear();
            }

            _sceneId = sceneId;
            _hasTiles = tiles is not null;
            _layerId = tiles?.LayerId;
            // Leaving the meeting ends it.
            Members.RemoveAll(id => !present.Contains(id));
            if (tiles is null)
            {
                return;
            }

            foreach (var member in tiles.Members)
            {
                if (ZoomSourceSetPolicy.ZoomMemberId(member) is { } id &&
                    present.Contains(id) &&
                    !Members.Contains(id, StringComparer.Ordinal))
                {
                    Members.Add(id);
                }
            }
        }

        private string? _layerId;
    }
}
