using CommunityToolkit.Mvvm.ComponentModel;
using CoreVideoPro.MediaCore.Contracts;

namespace CoreVideoPro.WinUI.ViewModels.ShowInputs;

/// <summary>One fixed Multiview slot's observed cells. No media payload lives here.</summary>
public sealed partial class MultiviewInputRow : ObservableObject
{
    public MultiviewInputRow(int slotNumber) => SlotNumber = slotNumber;

    public int SlotNumber { get; }
    public SourceInstanceIdentity? LastAppliedSourceInstance { get; internal set; }
    public string? RosterEpoch { get; internal set; }
    public long HighestObservationRevision { get; internal set; }
    public int HighestFrameId { get; internal set; }
    public double LastFrameAtMs { get; internal set; } = -1;

    [ObservableProperty] private string? _sourceId;
    [ObservableProperty] private string _statusLabel = "IDLE";
    [ObservableProperty] private string _formatLabel = "—";
    [ObservableProperty] private string _configuredCapLabel = "";
    [ObservableProperty] private bool _capDiffers;
    [ObservableProperty] private bool _formatStale;
    [ObservableProperty] private double _frameAgeMs = -1;
}
