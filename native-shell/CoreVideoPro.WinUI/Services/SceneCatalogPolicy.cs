using CoreVideoPro.WinUI.Models;

namespace CoreVideoPro.WinUI.Services;

/// <summary>
/// Scene removal and catalog-restore rules (#1070). Any scene that is not on air and not the
/// last visible scene can be removed, catalog or custom. The five catalog scenes are seeded
/// from <see cref="ProductionCatalog.Scenes"/> on every launch, so a removed catalog scene is
/// persisted by id (<c>RemovedCatalogSceneIds</c>) and dropped again at launch; "Restore
/// starter scenes" clears that set. Pure: no ViewModel, so the rules carry unit tests.
/// </summary>
public static class SceneCatalogPolicy
{
    public const string OnAirRefusal = "The on-air scene cannot be removed";
    public const string LastSceneRefusal = "The last scene cannot be removed";

    public static bool IsCatalogScene(string sceneId) =>
        ProductionCatalog.Scenes.Any(scene => string.Equals(scene.Id, sceneId, StringComparison.Ordinal));

    /// <summary>Why a scene cannot be removed right now, or null when it can. Internal
    /// (hidden multiview-solo) scenes never count toward the last-scene guard.</summary>
    public static string? RemovalRefusal(string sceneId, string activeSceneId, int visibleSceneCount, bool isInternalScene)
    {
        if (string.Equals(sceneId, activeSceneId, StringComparison.Ordinal))
        {
            return OnAirRefusal;
        }

        if (!isInternalScene && visibleSceneCount <= 1)
        {
            return LastSceneRefusal;
        }

        return null;
    }

    /// <summary>Catalog ids that are absent from the live scene list: what the profile persists.</summary>
    public static List<string> RemovedCatalogSceneIds(IEnumerable<Scene> scenes)
    {
        var present = scenes.Select(scene => scene.Id).ToHashSet(StringComparer.Ordinal);
        return ProductionCatalog.Scenes
            .Select(scene => scene.Id)
            .Where(id => !present.Contains(id))
            .ToList();
    }

    /// <summary>
    /// The persisted removals that still apply at launch, in catalog order. Unknown ids are
    /// ignored; a removal that would empty the visible scene list is skipped so the rail never
    /// ends up with nothing to cue.
    /// </summary>
    public static List<string> RemovalsToApply(
        IReadOnlyList<Scene> scenes,
        IEnumerable<string> removedCatalogSceneIds,
        Func<string, bool> isInternalScene)
    {
        var wanted = removedCatalogSceneIds
            .Where(id => !string.IsNullOrWhiteSpace(id))
            .ToHashSet(StringComparer.Ordinal);
        var visibleRemaining = scenes.Count(scene => !isInternalScene(scene.Id));
        var result = new List<string>();
        foreach (var catalogScene in ProductionCatalog.Scenes)
        {
            if (!wanted.Contains(catalogScene.Id) ||
                !scenes.Any(scene => string.Equals(scene.Id, catalogScene.Id, StringComparison.Ordinal)))
            {
                continue;
            }

            if (visibleRemaining <= 1)
            {
                break;
            }

            result.Add(catalogScene.Id);
            visibleRemaining--;
        }

        return result;
    }

    /// <summary>Catalog scenes missing from the live list, in catalog order, each paired with
    /// the index it should be re-inserted at so the rail keeps the starter order.</summary>
    public static List<(Scene Scene, int InsertIndex)> MissingCatalogScenes(IReadOnlyList<Scene> scenes)
    {
        var present = scenes.Select(scene => scene.Id).ToList();
        var missing = new List<(Scene, int)>();
        var catalogIndex = 0;
        foreach (var catalogScene in ProductionCatalog.Scenes)
        {
            if (!present.Contains(catalogScene.Id, StringComparer.Ordinal))
            {
                // Insert after the last catalog scene that precedes it and is present.
                var insertAt = 0;
                for (var i = 0; i < catalogIndex; i++)
                {
                    var earlier = present.IndexOf(ProductionCatalog.Scenes[i].Id);
                    if (earlier >= 0)
                    {
                        insertAt = Math.Max(insertAt, earlier + 1);
                    }
                }

                missing.Add((catalogScene, Math.Min(insertAt, present.Count)));
                present.Insert(Math.Min(insertAt, present.Count), catalogScene.Id);
            }

            catalogIndex++;
        }

        return missing;
    }

    /// <summary>
    /// The scene a show-type recommendation should cue when its preferred catalog scene has
    /// been removed: the preferred id when present, otherwise the first scene.
    /// </summary>
    public static string ResolveRecommendedSceneId(IReadOnlyList<Scene> scenes, string preferredSceneId)
    {
        if (scenes.Count == 0 || scenes.Any(scene => string.Equals(scene.Id, preferredSceneId, StringComparison.Ordinal)))
        {
            return preferredSceneId;
        }

        return scenes[0].Id;
    }
}
