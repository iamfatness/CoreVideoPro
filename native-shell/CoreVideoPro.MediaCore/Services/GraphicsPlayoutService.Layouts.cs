using CoreVideoPro.MediaCore.Models;

namespace CoreVideoPro.MediaCore.Services;

public sealed record GraphicsLayoutCue(string InstanceId, long CueRevision);

public sealed record GraphicsPreparedLayout(
    ShotLayout Layout, string CoreEpoch, string AttemptId, IReadOnlyList<GraphicsLayoutCue> Cues, string? PreviewStamp);

public sealed partial class GraphicsPlayoutService
{
    private CancellationTokenSource? _layoutPreparation;
    private GraphicsPreparedLayout? _preparedLayout;
    public GraphicsPreparedLayout? PreparedLayout => _preparedLayout;
    public bool LayoutPreparing => _layoutPreparation is not null;

    public async Task<NativeGraphicsOperationResult> CancelLayoutAsync()
    {
        _layoutPreparation?.Cancel();
        return await Run(async () =>
        {
            if (_preparedLayout is { } prepared)
            {
                _preparedLayout = null;
                await ReleaseLayoutCues(prepared.CoreEpoch, prepared.AttemptId, prepared.Cues);
            }
            return new() { Accepted = true, Reason = "Shot Layout cue cancelled." };
        });
    }

    private async Task<NativeGraphicsOperationResult> PrepareLayout(
        ShotLayout layout, IReadOnlyList<GraphicsPlaylistEntry> entries,
        IReadOnlyList<GraphicsLayoutResolver.ResolvedRow> rows,
        Func<string, CancellationToken, Task<bool>> sceneSelect, CancellationToken cancellation, Func<string>? previewStamp)
    {
        if (_preparedLayout is not null) return Refuse("Cancel the prepared Shot Layout before cueing another.");
        var state = Available();
        var instanceIds = entries.Select(InstanceIdFor).ToHashSet(StringComparer.Ordinal);
        if (state.Instances.Any(instance => instanceIds.Contains(instance.InstanceId) && (instance.Cued || instance.CuePending || IsOnPreview(instance))))
            return Refuse($"{layout.Name}: a required graphic already has a Preview cue. Clear it before cueing this layout.");

        var epoch = state.CoreEpoch;
        var attemptId = "layout-" + Guid.NewGuid().ToString("N");
        var cues = new List<GraphicsLayoutCue>(entries.Count);
        using var preparation = CancellationTokenSource.CreateLinkedTokenSource(cancellation);
        _layoutPreparation = preparation;
        Changed?.Invoke(this, EventArgs.Empty);
        var token = preparation.Token;
        try
        {
            if (layout.SceneId.Length > 0 && !await sceneSelect(layout.SceneId, token))
                return Refuse($"{layout.Name}: the selected scene is unavailable.");
            var stamp = previewStamp?.Invoke();
            var deadline = _utcNow().AddSeconds(5);
            for (var index = 0; index < entries.Count; index++)
            {
                token.ThrowIfCancellationRequested();
                Current(epoch);
                var cued = await Cue(entries[index], token, rows[index], attemptId, deadline, cues);
                if (!cued.Accepted) return Refuse($"{layout.Name}: {rows[index].Row.Name}: {cued.Reason}");
            }
            token.ThrowIfCancellationRequested();
            var latest = Current(epoch);
            if (cues.Any(cue => !latest.Instances.Any(instance => MatchesLayoutCue(instance, attemptId, cue))))
                return Refuse($"{layout.Name}: a prepared graphic changed. Cue the layout again.");
            if (stamp != previewStamp?.Invoke())
                return Refuse($"{layout.Name}: Preview changed during preparation. Cue the layout again.");
            _preparedLayout = new(layout, epoch, attemptId, cues.ToArray(), stamp);
            return new() { Accepted = true, Reason = $"{layout.Name} ready for Take." };
        }
        finally
        {
            if (_preparedLayout?.AttemptId != attemptId)
                await ReleaseLayoutCues(epoch, attemptId, cues);
            _layoutPreparation = null;
        }
    }

    private static bool MatchesLayoutCue(NativeGraphicsInstance instance, string owner, GraphicsLayoutCue cue) =>
        instance.InstanceId == cue.InstanceId && instance.CueOwner == owner && instance.CueRevision == cue.CueRevision &&
        instance.Cued && !instance.CuePending && instance.CuePreparation != "failed";

    private async Task ReleaseLayoutCues(string epoch, string attemptId, IReadOnlyList<GraphicsLayoutCue> cues)
    {
        if (_observe()?.CoreEpoch != epoch) return;
        _epoch = epoch;
        foreach (var cue in cues)
        {
            try
            {
                await Send("graphics.instance.trigger", new()
                {
                    ["instanceId"] = cue.InstanceId, ["expectedRevision"] = cue.CueRevision,
                    ["cueOwner"] = attemptId, ["action"] = "uncue"
                }, CancellationToken.None);
            }
            catch (Exception ex) when (ex is InvalidOperationException or OperationCanceledException)
            {
                // The guarded command can never clear another attempt's cue.
            }
        }
    }
}
