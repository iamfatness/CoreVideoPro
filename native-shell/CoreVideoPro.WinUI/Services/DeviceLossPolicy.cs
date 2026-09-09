using System;

namespace CoreVideoPro.WinUI.Services;

/// <summary>
/// Pure classification of a D3D/DXGI failure into "the device itself is gone" versus
/// "this present did not commit". GPU-free so it is unit-testable (see
/// <c>DeviceLossPolicyTests</c>) — the same shape as <see cref="PresentationAttempt"/>,
/// <c>NativeUvcCapturePolicy</c> and <c>CaptureReaderStallPolicy</c>.
///
/// WHY THIS EXISTS (beta slice, 2026-09-09): before this, a lost device threw out of
/// <see cref="PresentationAttempt.Commit"/>, the catch dropped that ONE host to CPU
/// fallback, and <c>s_sharedDevice</c> stayed non-null forever — so <c>EnsureDevice</c>
/// kept returning true for a dead device and EVERY surface stayed on CPU until the app
/// was restarted. Nothing in the tree ever called <c>GetDeviceRemovedReason</c>, so a
/// tester's log never named the cause. A TDR, a driver upgrade mid-show or a hardware
/// fault all looked identical: "the picture went soft and stayed soft".
/// </summary>
internal static class DeviceLossPolicy
{
    // The two failures that invalidate the DEVICE. Everything else (WAS_STILL_DRAWING,
    // occlusion, an out-of-memory swap-chain create, a bad shared handle) leaves the
    // device usable and must NOT trigger a retirement.
    internal const int DeviceRemoved = unchecked((int)0x887A0005); // DXGI_ERROR_DEVICE_REMOVED
    internal const int DeviceReset = unchecked((int)0x887A0007);   // DXGI_ERROR_DEVICE_RESET

    // Never a present result — these are only ever REASONS reported by GetDeviceRemovedReason.
    internal const int DeviceHung = unchecked((int)0x887A0006);            // DXGI_ERROR_DEVICE_HUNG
    internal const int DriverInternalError = unchecked((int)0x887A0020);   // DXGI_ERROR_DRIVER_INTERNAL_ERROR
    internal const int InvalidCall = unchecked((int)0x887A0001);           // DXGI_ERROR_INVALID_CALL
    internal const int AccessLost = unchecked((int)0x887A0026);            // DXGI_ERROR_ACCESS_LOST

    /// <summary>True when this HRESULT means the shared device must be retired and rebuilt.</summary>
    internal static bool IsDeviceLoss(int hr) => hr == DeviceRemoved || hr == DeviceReset;

    /// <summary>
    /// True when <c>GetDeviceRemovedReason</c> says the device is gone. S_OK (and any
    /// non-negative value) means the device is healthy and this failure was something else
    /// — a resource-pressure create failure, a stale shared handle — which must stay on the
    /// existing per-handle invalidation path rather than tearing down the process-wide device.
    /// </summary>
    internal static bool IsRemovedReasonFatal(int removedReason) => removedReason < 0;

    /// <summary>
    /// The line a support bundle is read for. The reason code is the ONLY thing that
    /// distinguishes a TDR from a driver upgrade from a dying card after the fact, so it is
    /// always logged in both hex and words.
    /// </summary>
    internal static string DescribeRemovedReason(int removedReason) => removedReason switch
    {
        0 => "S_OK (device not removed)",
        DeviceHung => "DEVICE_HUNG (our own workload hung the GPU — a TDR we caused)",
        DeviceRemoved => "DEVICE_REMOVED (adapter disappeared: driver upgrade, eGPU unplug, or hardware fault)",
        DeviceReset => "DEVICE_RESET (TDR triggered by another application on this GPU)",
        DriverInternalError => "DRIVER_INTERNAL_ERROR (driver fault — expect a driver update to be the fix)",
        InvalidCall => "INVALID_CALL (invalid API usage put the device in an unusable state)",
        AccessLost => "ACCESS_LOST (shared-resource access revoked, e.g. desktop switch)",
        _ => removedReason < 0 ? "unrecognized device-removed reason" : "non-fatal reason"
    };

    /// <summary>
    /// Bounded recovery ladder for shared-device recreation. Deliberately the SAME shape as
    /// <c>ShowEngineRestartPolicy</c> / <c>MediaCoreSupervisor</c> / <c>BrowserHostRestartPolicy</c> /
    /// <c>PluginHostRespawnPolicy</c>: escalating backoff, a hard consecutive-failure ceiling,
    /// and a healthy-run reset. Only the ladder VALUES differ, because recreating a D3D device
    /// costs milliseconds rather than a process launch.
    ///
    /// The ceiling is what stops a wedged GPU from turning into a per-vsync recreate loop —
    /// which would be both useless and, per CLAUDE.md, its own 0xc000027b churn vector.
    /// </summary>
    internal sealed class DeviceRecoveryPolicy
    {
        /// <summary>Backoff ladder in milliseconds: 250, 1s, 2s, 5s, 10s, then 30s repeating.</summary>
        internal static readonly TimeSpan[] Delays =
        {
            TimeSpan.FromMilliseconds(250),
            TimeSpan.FromSeconds(1),
            TimeSpan.FromSeconds(2),
            TimeSpan.FromSeconds(5),
            TimeSpan.FromSeconds(10),
            TimeSpan.FromSeconds(30),
        };

        private readonly int _maxConsecutiveFailures;
        private readonly TimeSpan _healthyResetAfter;
        private readonly object _gate = new();

        private int _consecutiveFailures;

        /// <summary>Set by <see cref="RecordRunning"/>, cleared by <see cref="NextDelay"/> and
        /// <see cref="Reset"/>. <see cref="RecordHealthy"/> is a no-op while this is null so a
        /// present heartbeat racing a loss can never forgive a budget the device never earned.</summary>
        private DateTimeOffset? _runningSince;

        internal DeviceRecoveryPolicy(int maxConsecutiveFailures = 5, TimeSpan? healthyResetAfter = null)
        {
            _maxConsecutiveFailures = maxConsecutiveFailures;
            _healthyResetAfter = healthyResetAfter ?? TimeSpan.FromSeconds(60);
        }

        /// <summary>How long to wait before attempting recreation, or null to give up (stay on
        /// CPU fallback until an operator-initiated <see cref="Reset"/>).</summary>
        internal TimeSpan? NextDelay(DateTimeOffset now)
        {
            lock (_gate)
            {
                _runningSince = null;
                _consecutiveFailures++;
                if (_consecutiveFailures > _maxConsecutiveFailures) return null;
                return Delays[Math.Min(_consecutiveFailures - 1, Delays.Length - 1)];
            }
        }

        /// <summary>A device was created; the clock for "healthy long enough" starts.</summary>
        internal void RecordRunning(DateTimeOffset now)
        {
            lock (_gate) { _runningSince = now; }
        }

        /// <summary>A GPU present succeeded at <paramref name="now"/>. Clears the failure budget
        /// once the CURRENT device has been presenting for <c>healthyResetAfter</c>.</summary>
        internal void RecordHealthy(DateTimeOffset now)
        {
            lock (_gate)
            {
                if (_runningSince is not { } since) return;
                if (now - since >= _healthyResetAfter) _consecutiveFailures = 0;
            }
        }

        internal int ConsecutiveFailures { get { lock (_gate) return _consecutiveFailures; } }

        /// <summary>Operator-initiated retry: forget the failure history.</summary>
        internal void Reset()
        {
            lock (_gate)
            {
                _consecutiveFailures = 0;
                _runningSince = null;
            }
        }
    }
}
