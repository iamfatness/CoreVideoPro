using CoreVideoPro.MediaCore.Models;

namespace CoreVideoPro.MediaCore.Services;

/// <summary>The one evictor for the 16-definition native library (#1087 (c)): the design lifecycle and
/// playout's pin reinstall both run it when an install meets a full library. It removes only design
/// template versions (<see cref="GraphicsDesignIds.IsDesignTemplateId"/>), never a curated or imported
/// look, and keeps every version something needs: document references (playlist rows, saved cues,
/// Studio lower third), live instances (draft, cue and program versions and images), the operator's
/// open draft, and remembered drafts that hold unsaved template edits.</summary>
internal static class GraphicsDesignEviction
{
    /// <summary>Versions in use: document references plus every live instance's draft, cue and program
    /// versions and images.</summary>
    public static HashSet<(string Id, int Version)> InUse(GraphicsShowDocument document, NativeGraphicsState state)
    {
        var pins = GraphicsWorkspaceLibrary.ReferencedTemplates(document);
        foreach (var instance in state.Instances) {
            foreach (var version in new[] { instance.TemplateVersion, instance.CueTemplateVersion, instance.ProgramTemplateVersion }.Where(v => v > 0))
                pins.Add((instance.TemplateId, version));
            foreach (var image in new[] { instance.Image, instance.CueImage, instance.ProgramImage })
                if (image is not null) pins.Add((image.TemplateId, image.TemplateVersion));
        }
        return pins;
    }

    /// <summary>Remove every installed design version nothing needs. Returns how many were removed.</summary>
    public static async Task<int> EvictUnusedAsync(GraphicsWorkspaceLibrary library, NativeGraphicsState state,
        Func<string, Dictionary<string, object?>, CancellationToken, Task<NativeGraphicsOperationResult>> send,
        Func<TimeSpan, CancellationToken, Task> delay, CancellationToken ct)
    {
        var keep = InUse(library.Document, state);
        if (library.EditorDraft is { } draft) keep.Add((draft.TemplateId, draft.TemplateVersion));
        keep.UnionWith(library.UnsavedTemplateEdits);
        var removed = 0;
        foreach (var template in state.Templates.Where(t => GraphicsDesignIds.IsDesignTemplateId(t.TemplateId) && !keep.Contains((t.TemplateId, t.TemplateVersion))).ToArray())
            if ((await RemoveAsync(send, delay, template.TemplateId, template.TemplateVersion, ct)).Accepted) removed++;
        return removed;
    }

    /// <summary>Native remove; template-library-busy never blocks, so retry it briefly (3 × 100 ms).</summary>
    public static async Task<NativeGraphicsOperationResult> RemoveAsync(
        Func<string, Dictionary<string, object?>, CancellationToken, Task<NativeGraphicsOperationResult>> send,
        Func<TimeSpan, CancellationToken, Task> delay, string templateId, int version, CancellationToken ct)
    {
        for (var attempt = 0; ; attempt++) {
            var result = await send("graphics.template.remove", new() { ["templateId"] = templateId, ["templateVersion"] = version }, ct);
            if (result.Accepted || result.Reason != "template-library-busy" || attempt == 3) return result;
            await delay(TimeSpan.FromMilliseconds(100), ct);
        }
    }
}
