using CoreVideoPro.MediaCore.Models;

namespace CoreVideoPro.MediaCore.Services;

/// <summary>Resolve a lost command reply from the authoritative bounded result history.</summary>
public static class ControlOperationResultPolicy
{
    public static NativeMediaCoreAudioRouteResult? AudioRoute(
        NativeMediaCoreAudioRouteControl? control, string? operationId)
    {
        if (control is null || string.IsNullOrWhiteSpace(operationId)) return null;
        return control.LastResult?.OperationId == operationId &&
            control.LastResult.AuthorityEpoch == control.AuthorityEpoch
            ? control.LastResult
            : control.RecentResults.FirstOrDefault(result =>
                result.OperationId == operationId && result.AuthorityEpoch == control.AuthorityEpoch);
    }

    public static NativeMediaCoreMonitorControlResult? Monitor(
        NativeMediaCoreMonitorControl? control, string? operationId)
    {
        if (control is null || string.IsNullOrWhiteSpace(operationId)) return null;
        return control.LastResult?.OperationId == operationId &&
            control.LastResult.AuthorityEpoch == control.AuthorityEpoch
            ? control.LastResult
            : control.RecentResults.FirstOrDefault(result =>
                result.OperationId == operationId && result.AuthorityEpoch == control.AuthorityEpoch);
    }
}
