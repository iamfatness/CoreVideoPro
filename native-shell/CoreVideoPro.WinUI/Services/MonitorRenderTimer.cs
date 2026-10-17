using System.ComponentModel;
using System.Runtime.InteropServices;
using Microsoft.Win32.SafeHandles;

namespace CoreVideoPro.WinUI.Services;

/// <summary>A private high-resolution wait; does not change the process/system timer resolution.</summary>
internal sealed class MonitorRenderTimer : IDisposable
{
    private readonly EventWaitHandle _timer = new(false, EventResetMode.AutoReset);
    private readonly WaitHandle[] _waits;

    internal MonitorRenderTimer(WaitHandle wake)
    {
        var handle = CreateWaitableTimerExW(0, null, 2, 0x1F0003);
        if (handle == 0) handle = CreateWaitableTimerExW(0, null, 0, 0x1F0003);
        if (handle == 0) throw new Win32Exception(Marshal.GetLastWin32Error());
        _timer.SafeWaitHandle = new SafeWaitHandle(handle, ownsHandle: true);
        _waits = [wake, _timer];
    }

    internal void Wait(long delay100ns)
    {
        var due = -Math.Max(1, delay100ns);
        if (!SetWaitableTimer(_timer.SafeWaitHandle, ref due, 0, 0, 0, false))
            throw new Win32Exception(Marshal.GetLastWin32Error());
        WaitHandle.WaitAny(_waits);
    }

    public void Dispose() => _timer.Dispose();

    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    private static extern nint CreateWaitableTimerExW(nint attributes, string? name, uint flags, uint access);
    [DllImport("kernel32.dll", SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool SetWaitableTimer(SafeWaitHandle timer, ref long dueTime, int period,
        nint completion, nint argument, [MarshalAs(UnmanagedType.Bool)] bool resume);
}
