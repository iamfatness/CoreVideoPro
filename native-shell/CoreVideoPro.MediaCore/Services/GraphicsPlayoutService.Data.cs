using CoreVideoPro.MediaCore.Models;

namespace CoreVideoPro.MediaCore.Services;

public sealed partial class GraphicsPlayoutService
{
    public Task<NativeGraphicsOperationResult> PublishDataAsync(string rowId, CancellationToken cancellation = default) => Run(async () =>
    {
        await _library.LoadAsync();
        if (Row(rowId) is not { DataBinding: { } binding } row ||
            _library.Document.DataSources.FirstOrDefault(source => source.Id == binding.SourceId) is not { } source)
            return Refuse("This row has no local dataset.");
        if (source.Revision < binding.Revision) return Refuse("The published dataset revision is unavailable.");

        Dictionary<string, System.Text.Json.JsonElement> cells;
        try { cells = GraphicsDataTable.Fields(source.Data, binding); }
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
