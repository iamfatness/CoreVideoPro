using System;
using System.Linq;
using System.Runtime.InteropServices;
using System.Threading;

namespace CoreVideoPro.WinUI.Services;

/// <summary>
/// Test-only fault seams for the shell's PRESENTATION path.
///
/// WHY THIS EXISTS (beta slice, 2026-09-09). Two gates in
/// <c>docs/production-realtime-execution-plan.md</c> are written in terms of injected
/// faults — G2 wants "monitor rendering and shell presentation fault-injected
/// independently", G3 wants writer hangs and storage faults shown to stay isolated — and
/// nothing in this tree could inject anything. The concrete customer is the D3D
/// device-loss recovery that shipped the same day: the retirement ladder, the generation
/// bump, the swap-chain rebuild and the handle-blacklist clear are all real code that had
/// never been executed, because a device-removed result cannot be produced on demand
/// without provoking a TDR. This is the seam that executes it.
///
/// GUARDING — this must be impossible to trip in a shipped build:
///  * <c>internal</c>, and the WinUI assembly's only <c>InternalsVisibleTo</c> is
///    <c>CoreVideoPro.WinUI.Tests</c>. No product code path calls any member here.
///  * Arming additionally REFUSES unless the test assembly is loaded in this process
///    (<see cref="RequireTestHost"/>). That check is a positive requirement and fails
///    closed, so a future caller that wandered into the shipped shell would get a loud
///    exception rather than a silently armed fault.
///  * There is no environment variable, control-API action, config key or wire field
///    that reaches it. Arming is an in-process call and nothing else — the same shape as
///    <c>MediaCore::setStillImageDecoderForTest</c>, this repo's existing injection point.
///  * Every arming API returns an <see cref="IDisposable"/> that disarms, so a failing
///    test cannot leave a process-wide fault armed for the rest of the run.
///
/// COST WHEN DISABLED — the present path reads exactly one static bool
/// (<see cref="Armed"/>). No allocation, no lock, no environment read, no delegate
/// invocation. Everything else in this class is behind that branch.
///
/// THREADING — <see cref="Armed"/> is written with <c>Volatile.Write</c> AFTER the fault
/// state it guards is published, and cleared BEFORE that state is torn down, so a present
/// thread that observes <c>true</c> always observes consistent fault state.
/// </summary>
internal static class PresentationFaultInjection
{
    private static bool s_armed;

    private static int s_pendingDeviceLossHResult;
    private static int s_pendingDeviceLossCount;
    private static ManualResetEventSlim? s_presentGate;
    private static int s_presentGateTimeoutMs;
    private static int s_presentEntries;
    private static int s_presentsBlocked;

    /// <summary>The ONE thing the present hot path reads.</summary>
    internal static bool Armed => Volatile.Read(ref s_armed);

    /// <summary>How many times the present seam has been reached while armed. Lets a test
    /// prove the fault actually fired rather than assuming it did.</summary>
    internal static int PresentEntries => Volatile.Read(ref s_presentEntries);

    /// <summary>How many presents are currently sitting inside an injected block.</summary>
    internal static int PresentsBlocked => Volatile.Read(ref s_presentsBlocked);

    /// <summary>
    /// Fails closed unless this process is a test host. A shipped build cannot get past
    /// this, whatever a future caller intends.
    /// </summary>
    private static void RequireTestHost()
    {
        var loaded = AppDomain.CurrentDomain.GetAssemblies()
            .Any(assembly => assembly.GetName().Name == "CoreVideoPro.WinUI.Tests");
        if (!loaded)
        {
            throw new InvalidOperationException(
                "PresentationFaultInjection is a test-only seam and refuses to arm outside the " +
                "CoreVideoPro.WinUI.Tests host. If you are seeing this in the product, a fault " +
                "seam has been wired into a shipping code path — that is the bug.");
        }
    }

    /// <summary>
    /// The next <paramref name="occurrences"/> presents fail with <paramref name="hresult"/>
    /// (default <c>DXGI_ERROR_DEVICE_REMOVED</c>), thrown from the real present seam so the
    /// existing catch classifies it, retires the device generation and starts the recovery
    /// ladder — exactly the path a real TDR takes.
    /// </summary>
    internal static IDisposable ArmDeviceLossForTest(
        int hresult = DeviceLossPolicy.DeviceRemoved, int occurrences = 1)
    {
        RequireTestHost();
        Volatile.Write(ref s_presentEntries, 0);
        Volatile.Write(ref s_pendingDeviceLossHResult, hresult);
        Volatile.Write(ref s_pendingDeviceLossCount, occurrences);
        Volatile.Write(ref s_armed, true);
        return new Disarm();
    }

    /// <summary>
    /// Every present blocks until the returned handle's <c>Release()</c> is called, or
    /// until <paramref name="hardTimeout"/> elapses — whichever comes first.
    ///
    /// The hard timeout is not a convenience: presents currently run on the UI thread from
    /// <c>CompositionTarget.Rendering</c>, so an UNBOUNDED block here would wedge whatever
    /// thread drives the seam with no way back. A fault seam that can hang the process it
    /// is meant to diagnose is not a diagnostic.
    /// </summary>
    internal static PresentBlock ArmBlockedPresentForTest(TimeSpan hardTimeout)
    {
        RequireTestHost();
        Volatile.Write(ref s_presentEntries, 0);
        var gate = new ManualResetEventSlim(false);
        Volatile.Write(ref s_presentGateTimeoutMs, (int)Math.Max(1, hardTimeout.TotalMilliseconds));
        Volatile.Write(ref s_presentGate, gate);
        Volatile.Write(ref s_armed, true);
        return new PresentBlock(gate);
    }

    /// <summary>Runs at the real present seam. Never called unless <see cref="Armed"/>.</summary>
    internal static void BeforePresent()
    {
        Interlocked.Increment(ref s_presentEntries);

        if (Volatile.Read(ref s_pendingDeviceLossCount) > 0)
        {
            Interlocked.Decrement(ref s_pendingDeviceLossCount);
            Marshal.ThrowExceptionForHR(Volatile.Read(ref s_pendingDeviceLossHResult));
        }

        if (Volatile.Read(ref s_presentGate) is { } gate)
        {
            Interlocked.Increment(ref s_presentsBlocked);
            try
            {
                gate.Wait(Volatile.Read(ref s_presentGateTimeoutMs));
            }
            catch (ObjectDisposedException)
            {
                // The block was released and disposed while this present was waiting.
                // Releasing is the intended end state, so this is not a failure.
            }
            finally
            {
                Interlocked.Decrement(ref s_presentsBlocked);
            }
        }
    }

    /// <summary>Clears every armed fault and every counter. Called by the disposables, and
    /// by tests between cases so process-wide state cannot leak between them.</summary>
    internal static void ResetForTest()
    {
        RequireTestHost();
        Volatile.Write(ref s_armed, false);
        var gate = Interlocked.Exchange(ref s_presentGate, null);
        gate?.Set();
        gate?.Dispose();
        Volatile.Write(ref s_pendingDeviceLossCount, 0);
        Volatile.Write(ref s_pendingDeviceLossHResult, 0);
        Volatile.Write(ref s_presentEntries, 0);
    }

    /// <summary>Handle for a blocked present: <see cref="Release"/> lets the blocked
    /// present through, <see cref="Dispose"/> disarms the seam entirely.</summary>
    internal sealed class PresentBlock : IDisposable
    {
        private readonly ManualResetEventSlim _gate;

        internal PresentBlock(ManualResetEventSlim gate) => _gate = gate;

        internal void Release() => _gate.Set();

        public void Dispose() => ResetForTest();
    }

    private sealed class Disarm : IDisposable
    {
        public void Dispose() => ResetForTest();
    }
}
