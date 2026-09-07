using CoreVideoPro.MediaCore.Models;
using CoreVideoPro.MediaCore.Services;

namespace CoreVideoPro.WinUI.Services;

// Constructed as the argument to StudioViewModel's delegating constructor, before
// its constructor body attaches callbacks that can launch the native process.
public sealed class StudioStartupBootstrap
{
    private StudioStartupBootstrap(IProductionOutputPreferencesStore store, MediaCoreBridgeService bridge,
        ProductionPreferencesLoadResult loaded, Exception? loadError)
    {
        Store = store;
        Bridge = bridge;
        Loaded = loaded;
        LoadError = loadError;
        ProgramBufferFrames = ProgramBufferPreference.Normalize(loaded.Preferences?.ProgramBufferFrames ?? ProgramBufferPreference.DefaultFrames);
        Bridge.ConfigureProgramBufferFrames(ProgramBufferFrames);
    }

    public IProductionOutputPreferencesStore Store { get; }
    public MediaCoreBridgeService Bridge { get; }
    public ProductionPreferencesLoadResult Loaded { get; }
    public Exception? LoadError { get; }
    public int ProgramBufferFrames { get; }

    public static StudioStartupBootstrap Create(IProductionOutputPreferencesStore store, MediaCoreBridgeService? bridge = null)
    {
        ProductionPreferencesLoadResult loaded;
        Exception? error = null;
        try
        {
            loaded = store is FileProductionOutputPreferencesStore fileStore ? fileStore.LoadWithResult()
                : new ProductionPreferencesLoadResult(ProductionPreferencesLoadStatus.Loaded, store.Load());
        }
        catch (Exception ex)
        {
            error = ex;
            loaded = new ProductionPreferencesLoadResult(ProductionPreferencesLoadStatus.Unreadable, null);
        }
        return new StudioStartupBootstrap(store, bridge ?? new MediaCoreBridgeService(), loaded, error);
    }

    public void Restore(Action<ProductionOutputPreferences> apply)
    {
        if (Loaded.Preferences is { } preferences) apply(preferences);
    }
}
