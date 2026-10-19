namespace CoreVideoPro.MediaCore.Services;

public sealed record GraphicsStarterWorkflowResult(bool Added, bool Saved, bool StudioAssigned, string Message);

/// <summary>The explicit choose-look action: hidden adoption, persistence, then
/// independent Studio assignment. Never Cue, Show, enable outputs or automation.</summary>
public static class GraphicsStarterWorkflow
{
    public static async Task<GraphicsStarterWorkflowResult> AddAsync(GraphicsWorkspaceSession session,
        GraphicsWorkspaceLibrary library, IReadOnlyList<GraphicsStarterGraphic> graphics,
        bool useInStudio, bool followProgramSource)
    {
        await library.LoadAsync();
        if (library.ReadOnly) return new(false, false, false, "Your Graphics document is read-only. The current Studio look was kept.");
        if (useInStudio && (graphics.Count == 0 || graphics[0].Template.Family != "lower-third"))
            return new(false, false, false, "Choose a show template with a guest introduction for Studio.");
        if (!await session.AddStarterAsync(graphics)) return new(false, false, false, session.Status);
        var firstId = session.SelectedId;
        var added = (session.State?.Instances ?? []).Where(i => session.LastAddedStarterIds.Contains(i.InstanceId, StringComparer.Ordinal)).ToArray();
        var saved = added.Length == graphics.Count;
        foreach (var item in added)
        {
            session.Select(item.InstanceId);
            if (session.SelectedId != item.InstanceId || session.Selected is null || !await library.SaveCueAsync(item.Label)) { saved = false; break; }
        }
        session.Select(firstId);
        if (!saved) return new(true, false, false, "The graphics were added hidden, but saving did not finish. The current Studio look was kept. " + library.Status);
        if (!useInStudio) return new(true, true, false, "Your four graphics are saved. Select Guest introduction and Use in Studio to assign its look, or use Cue and Show for individual graphics.");
        if (!await library.AssignStudioLowerThirdAsync(followProgramSource))
            return new(true, true, false, "Your graphics are saved, but Studio assignment did not finish. " + library.Status);
        return new(true, true, true, "Look assigned. Go to Studio and use the existing LT in control. Guest names follow the Program source when enabled; nothing was put on air.");
    }
}
