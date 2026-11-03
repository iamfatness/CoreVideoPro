namespace CoreVideoPro.Control;

/// <summary>Refuse-closed check for <c>scene.select</c> (#1070 review). Pure: no ViewModel, so
/// the rule is unit-tested here and the WinUI adapter only applies it.</summary>
public static class SceneSelectArgs
{
    /// <summary>Null when the scene may be cued. A blank or unknown id: a reason, and the
    /// caller must change nothing.</summary>
    public static string? Refuse(string? sceneId, IReadOnlyCollection<string> knownScenes)
    {
        if (string.IsNullOrWhiteSpace(sceneId) ||
            !knownScenes.Any(id => string.Equals(id, sceneId, StringComparison.Ordinal)))
        {
            return $"Unknown scene '{sceneId}'.";
        }

        return null;
    }
}
