namespace CoreVideoPro.WinUI.Services;

public static class ProgramBufferPreferencePersistence
{
    public static void Save(IProductionOutputPreferencesStore store, ProductionOutputPreferences preferences,
        int frames, Action<int> commitSelection)
    {
        if (frames is not (2 or 3)) throw new ArgumentOutOfRangeException(nameof(frames));
        preferences.ProgramBufferFrames = frames;
        store.Save(preferences);
        // Publish only after the atomic preferences write succeeds. A failed write
        // leaves the old selection intact, allowing an identical request to retry.
        commitSelection(frames);
    }
}
