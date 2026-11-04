using CoreVideoPro.MediaCore.Models;

namespace CoreVideoPro.MediaCore.Services;

public sealed partial class GraphicsPlayoutService
{
    private NativeGraphicsState? _observedGraphics;

    public NativeGraphicsInstance? CrawlOnProgram(string rowId) => FindCrawl(_observedGraphics, rowId);

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

    public Task<NativeGraphicsOperationResult> SetCrawlLiveFollowAsync(string rowId, bool enabled) => Run(async () =>
    {
        await _library.LoadAsync();
        if (Row(rowId) is not { DataBinding: { Crawl: not null } binding } row)
            return Refuse("This row has no crawl binding.");
        var saved = await _library.SavePlaylistAsync(GraphicsBuilder.Upsert(_library.Document.Playlist,
            row with { DataBinding = binding with { LiveFollow = enabled } }));
        return saved ? new() { Accepted = true, Reason = enabled ? "Live follow enabled for subsequent reloads." : "Manual publication enabled." }
            : Refuse(_library.Status);
    });

    public Task<NativeGraphicsOperationResult> PublishDataAsync(string rowId, CancellationToken cancellation = default) => Run(async () =>
    {
        await _library.LoadAsync();
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
            if (!await _library.SavePlaylistAsync(GraphicsBuilder.Upsert(_library.Document.Playlist, published)))
                return Refuse(_library.Status);
        }

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
    });
}
