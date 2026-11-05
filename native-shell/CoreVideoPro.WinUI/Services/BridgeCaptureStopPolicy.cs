namespace CoreVideoPro.WinUI.Services;

public enum CaptureBridgeSignal : uint
{
    Live = 1,
    Lost = 2,
    Stopped = 3
}

public enum BridgeCaptureStopReason
{
    ReaderLost,
    DeviceRemoved,
    OperatorDisconnect,
    NativeTakeover,
    Shutdown
}

public static class BridgeCaptureStopPolicy
{
    // Mirrors kCaptureSilentFailNs in native/src/core/CaptureLivenessPolicy.h.
    public const int CoreSilentFailMs = 5000;
    public static readonly TimeSpan BridgeLostReleaseDelay = TimeSpan.FromSeconds(6);

    public static (CaptureBridgeSignal Signal, TimeSpan ReleaseDelay) For(BridgeCaptureStopReason reason) =>
        reason is BridgeCaptureStopReason.ReaderLost or BridgeCaptureStopReason.DeviceRemoved
            ? (CaptureBridgeSignal.Lost, BridgeLostReleaseDelay)
            : (CaptureBridgeSignal.Stopped, TimeSpan.Zero);

    public static IEnumerable<string> VanishedBridgedDeviceIds(
        IEnumerable<string> prior, IEnumerable<string> discovered, IEnumerable<string> bridged) =>
        prior.Intersect(bridged, StringComparer.Ordinal).Except(discovered, StringComparer.Ordinal);
}
