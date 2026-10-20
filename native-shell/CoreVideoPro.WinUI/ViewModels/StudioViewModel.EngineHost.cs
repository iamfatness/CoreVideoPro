namespace CoreVideoPro.WinUI.ViewModels;

public sealed partial class StudioViewModel
{
    private bool ObserveExistingHostedShow()
    {
        if (!_bridge.Supervisor.HostedReadOnly) return false;
        RunOnUiThread(() =>
        {
            EngineStatus = "Attached to running show — read-only";
            CommandStatus = "Production continues independently. Saved settings were not applied. Editing a reattached show is pending state ownership migration.";
            RefreshSurfaceBindings();
            RefreshOutputStatus();
            RefreshTransportState();
        });
        return true;
    }

    private bool DetachHostedEngineOnExit()
    {
        if (!_bridge.Supervisor.Hosted) return false;
        try
        {
            _bridge.StopForAppExit(TimeSpan.Zero);
            LaunchLog.Write("shutdown: console detached; EngineHost retains production");
        }
        catch (Exception ex) { LaunchLog.Write($"shutdown: host detach failed ({ex.GetType().Name}); production was not stopped"); }
        return true; // Never fall through to the legacy kill-tree fallback.
    }
}
