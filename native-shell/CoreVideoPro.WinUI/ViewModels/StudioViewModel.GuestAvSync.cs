namespace CoreVideoPro.WinUI.ViewModels;

public sealed partial class StudioViewModel
{
    public GuestAvSyncController GuestAvSync { get; private set; } = null!;

    private void InitializeGuestAvSync()
    {
        GuestAvSync = new GuestAvSyncController(
            _bridge, () => SelectedParticipantId, () => IsPerGuestIsoAudio,
            () => ZoomOnline);
        PropertyChanged += (_, change) =>
        {
            if (change.PropertyName is nameof(SelectedParticipantId) or nameof(IsPerGuestIsoAudio))
                GuestAvSync.Refresh();
            else if (change.PropertyName == nameof(ZoomStatus))
            {
                if (!ZoomOnline) GuestAvSync.ClearSession();
                else GuestAvSync.Refresh();
            }
        };
    }
}
