using CoreVideoPro.MediaCore.Services;

namespace CoreVideoPro.WinUI.Services;

public static class EngineHostBootstrap
{
    public static MediaCoreBridgeService CreateBridge(bool interactive)
    {
        if (!interactive || Environment.GetEnvironmentVariable("COREVIDEO_ENGINE_HOST") != "1")
            return new MediaCoreBridgeService();
        var executable = Environment.GetEnvironmentVariable("COREVIDEO_ENGINE_HOST_PATH")
            ?? Path.Combine(AppContext.BaseDirectory, "CoreVideoPro.EngineHost.exe");
        if (!File.Exists(executable)) throw new FileNotFoundException("Build/stage the opt-in EngineHost first.", executable);
        var native = MediaCorePaths.ResolveNativeCoreExecutable()
            ?? throw new FileNotFoundException("Native media core unavailable.");
        var profile = Environment.GetEnvironmentVariable("COREVIDEO_ENGINE_HOST_PROFILE") ?? "default";
        return new MediaCoreBridgeService(new MediaCoreSupervisor(new MediaCoreSupervisorOptions
        {
            EngineHost = new EngineHostClientOptions(Path.GetFullPath(executable), native, profile)
        }));
    }
}
