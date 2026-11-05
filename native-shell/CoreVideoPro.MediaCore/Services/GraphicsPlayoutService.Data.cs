using CoreVideoPro.MediaCore.Models;

namespace CoreVideoPro.MediaCore.Services;

public sealed partial class GraphicsPlayoutService
{
    private NativeGraphicsState? _observedGraphics;

    public NativeGraphicsInstance? CrawlOnProgram(string rowId) => FindCrawl(_observedGraphics, rowId);
    public NativeGraphicsInstance? DataOnProgram(string rowId) =>
        _observedGraphics?.Instances.FirstOrDefault(instance => instance.InstanceId == InstanceIdFor(rowId, "") && IsOnProgram(instance));

    private static NativeGraphicsInstance? FindCrawl(NativeGraphicsState? state, string rowId) =>
        state?.Instances.FirstOrDefault(instance => instance.InstanceId == InstanceIdFor(rowId, "") &&
            IsOnProgram(instance) && instance.Visibility != "exiting" && instance.CrawlPlayback.Length > 0);

    public Task<NativeGraphicsOperationResult> ControlCrawlAsync(string rowId, string action, CancellationToken cancellation = default) => Run(async () =>
    {
        if (action is not ("pause" or "resume" or "restart") || _observe() is not { } state ||
            FindCrawl(state, rowId) is not { } live)
            return Refuse("This crawl is not on Program.");
        _epoch = state.CoreEpoch;
        return await Send("graphics.instance.trigger", new()
        {
            ["instanceId"] = live.InstanceId, ["expectedRevision"] = live.Revision, ["action"] = action
        }, cancellation);
    });

    public Task<NativeGraphicsOperationResult> SetDataLiveFollowAsync(string rowId, bool enabled) => Run(async () =>
    {
        await _library.LoadAsync();
        if (Row(rowId) is not { DataBinding: { } binding } row || (binding.Crawl is null && !binding.Remote))
            return Refuse("Live follow is unavailable for this row.");
        var saved = await _library.SavePlaylistAsync(GraphicsBuilder.Upsert(_library.Document.Playlist,
            row with { DataBinding = binding with { LiveFollow = enabled } }));
        return saved ? new() { Accepted = true, Reason = enabled ? "Live follow enabled for subsequent reloads." : "Manual publication enabled." }
            : Refuse(_library.Status);
    });

    public async Task<GraphicsFeedStage> StageFeedAsync(GraphicsFeedDefinition definition, GraphicsLocalData data,
        DateTimeOffset receivedAt, CancellationToken cancellation)
    {
        await _gate.WaitAsync(cancellation);
        try
        {
            await _library.LoadAsync();
            var rows = Playlist.Where(row => row.DataBinding?.SourceId == definition.Id).ToArray();
            try
            {
                foreach (var row in rows) GraphicsDataTable.Fields(data, row.DataBinding!);
            }
            catch (InvalidDataException) { return GraphicsFeedStage.Rejected; }

            var result = await _library.StageFeedAsync(definition, data, receivedAt, cancellation);
            if (result != GraphicsFeedStage.Rejected)
                await FollowRemoteFeedAsync(definition, cancellation);
            return result;
        }
        finally { _gate.Release(); }
    }

    public async Task<GraphicsFeedStage> StagePushSnapshotAsync(GraphicsFeedDefinition definition, GraphicsFeedSnapshot snapshot,
        DateTimeOffset receivedAt, CancellationToken cancellation)
    {
        await _gate.WaitAsync(cancellation);
        try
        {
            var result = await _library.StagePushSnapshotAsync(definition, snapshot, receivedAt, cancellation);
            if (result == GraphicsFeedStage.Updated)
            {
                try { await FollowRemoteFeedAsync(definition, cancellation); }
                catch (Exception ex) when (ex is not (OperationCanceledException or OutOfMemoryException))
                {
                    Status = "Feed saved. Program publication was interrupted.";
                    Changed?.Invoke(this, EventArgs.Empty);
                }
            }
            return result;
        }
        finally { _gate.Release(); }
    }

    private async Task FollowRemoteFeedAsync(GraphicsFeedDefinition definition, CancellationToken cancellation)
    {
        var revision = _library.Document.DataSources.Single(source => source.Id == definition.Id).Revision;
        foreach (var row in Playlist.Where(row => row.DataBinding?.SourceId == definition.Id).ToArray())
        {
            cancellation.ThrowIfCancellationRequested();
            if (!_library.Document.Feeds.Contains(definition)) break;
            if (Row(row.Id)?.DataBinding is not { LiveFollow: true } binding ||
                binding.SourceId != definition.Id || binding.Revision >= revision) continue;
            try { Status = (await PublishData(row.Id, cancellation, waitForProgram: false)).Reason; }
            catch (InvalidOperationException) { Status = "Feed saved. Program publication was interrupted."; }
            Changed?.Invoke(this, EventArgs.Empty);
        }
    }

    public Task<NativeGraphicsOperationResult> StageDataAsync(string rowId, GraphicsDataBinding selectedBinding,
        GraphicsLocalData data, CancellationToken cancellation = default) => Run(async () =>
    {
        await _library.LoadAsync();
        if (Row(rowId) is not { DataBinding: { } binding } row || binding != selectedBinding || binding.Remote)
            return Refuse("The row changed while the data file was open. Choose the file again.");

        try { GraphicsDataTable.Fields(data, binding); }
        catch (InvalidDataException ex) { return Refuse(ex.Message); }

        var source = _library.Document.DataSources.FirstOrDefault(item => item.Id == binding.SourceId);
        var staged = await _library.SaveLocalDataAsync(binding.SourceId, source?.Name ?? row.Name, data);
        if (staged is null) return Refuse(_library.Status);
        return binding.LiveFollow
            ? await PublishData(rowId, cancellation)
            : new() { Accepted = true, Reason = $"Data revision {staged.Revision} loaded. Choose Publish data to apply it." };
    });

    public Task<NativeGraphicsOperationResult> PublishDataAsync(string rowId, CancellationToken cancellation = default) => Run(async () =>
    {
        await _library.LoadAsync();
        return await PublishData(rowId, cancellation);
    });

    private async Task<NativeGraphicsOperationResult> PublishData(string rowId, CancellationToken cancellation, bool waitForProgram = true)
    {
        cancellation.ThrowIfCancellationRequested();
        if (Row(rowId) is not { DataBinding: { } binding } row ||
            _library.Document.DataSources.FirstOrDefault(source => source.Id == binding.SourceId) is not { } source)
            return Refuse("This row has no local dataset.");
        if (source.Revision < binding.Revision) return Refuse("The published dataset revision is unavailable.");

        Dictionary<string, System.Text.Json.JsonElement> cells;
        try { cells = GraphicsDataTable.Fields(source.Data, binding with { Revision = source.Revision }); }
        catch (InvalidDataException ex) { return Refuse(ex.Message); }
        var values = row.Fields.ToDictionary(StringComparer.Ordinal);
        foreach (var cell in cells) values[cell.Key] = cell.Value;
        if (source.Revision != binding.Revision)
        {
            var published = row with { Fields = values, DataBinding = binding with { Revision = source.Revision } };
            if (!await _library.SavePlaylistAsync(GraphicsBuilder.Upsert(_library.Document.Playlist, published), cancellation: cancellation))
                return Refuse(_library.Status);
        }

        cancellation.ThrowIfCancellationRequested();
        var state = _observe();
        if (state is null || state.CoreEpoch.Length == 0)
            return new() { Accepted = true, Reason = $"Data revision {source.Revision} saved for the next Cue or Show." };
        _epoch = state.CoreEpoch;
        var id = InstanceIdFor(row.Id, "");
        if (state.Instances.FirstOrDefault(instance => instance.InstanceId == id && IsOnProgram(instance)) is not { } live)
            return new() { Accepted = true, Reason = $"Data revision {source.Revision} saved for the next Cue or Show." };
        if (live.TemplateId != row.TemplateId || live.TemplateVersion != row.TemplateVersion || live.ProgramTemplateVersion != row.TemplateVersion)
            return Refuse("Data is saved. Show the current design version to put it on Program.");

        var updated = await Send("graphics.instance.update", new()
        {
            ["instanceId"] = id, ["expectedRevision"] = live.Revision, ["target"] = "program", ["fields"] = values
        }, cancellation);
        if (!updated.Accepted) return updated with { Reason = "Data is saved. Program update refused. " + updated.Reason };

        if (binding.Crawl is { } crawl)
            return updated with { Reason = crawl.Mode == "loop" ?
                $"Data revision {source.Revision} queued for a loop boundary." :
                $"Data revision {source.Revision} saved for the next Cue or Show." };

        if (!waitForProgram) return updated with { Reason = $"Data revision {source.Revision} submitted for Program preparation." };

        var until = _utcNow().AddSeconds(5);
        while (true)
        {
            var current = Current(state.CoreEpoch).Instances.FirstOrDefault(instance => instance.InstanceId == id);
            if (current?.ProgramRevision == updated.CurrentRevision && updated.CurrentRevision > 0)
                return updated with { Reason = $"Data revision {source.Revision} is on Program." };
            if (current is null || !IsOnProgram(current) || current.Revision > updated.CurrentRevision || current.Preparation == "failed")
                return Refuse("Data is saved. Program did not adopt the new revision. " + current?.Warning);
            if (_utcNow() >= until)
                return updated with { Reason = "Data is saved. The Program update is still preparing." };
            await _delay(TimeSpan.FromMilliseconds(50), cancellation);
        }
    }
}
