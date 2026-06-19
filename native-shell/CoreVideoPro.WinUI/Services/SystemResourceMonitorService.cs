using System.Runtime.InteropServices;

namespace CoreVideoPro.WinUI.Services;

/// <summary>
/// Polls Windows system CPU, memory, and disk utilization for transport readouts.
/// </summary>
public sealed class SystemResourceMonitorService : IDisposable
{
    private ulong _lastIdleTime;
    private ulong _lastKernelTime;
    private ulong _lastUserTime;
    private bool _hasCpuBaseline;
    private bool _disposed;

    public int CpuLoadPercent { get; private set; }
    public int MemoryLoadPercent { get; private set; }
    public int DiskLoadPercent { get; private set; }

    public event Action<int, int, int>? ResourcesSampled;

    public void Sample()
    {
        ObjectDisposedException.ThrowIf(_disposed, this);

        CpuLoadPercent = SampleCpuLoad();
        MemoryLoadPercent = SampleMemoryLoad();
        DiskLoadPercent = SampleDiskLoad();
        ResourcesSampled?.Invoke(CpuLoadPercent, MemoryLoadPercent, DiskLoadPercent);
    }

    /// <summary>Prime CPU baseline then return a live reading immediately.</summary>
    public void Prime()
    {
        ObjectDisposedException.ThrowIf(_disposed, this);

        Sample();
        Sample();
        if (CpuLoadPercent == 0)
        {
            // One more pass so transport bars show a value on cold start.
            Thread.Sleep(50);
            Sample();
        }
    }

    private int SampleCpuLoad()
    {
        if (!GetSystemTimes(out var idle, out var kernel, out var user))
        {
            return CpuLoadPercent;
        }

        if (!_hasCpuBaseline)
        {
            _lastIdleTime = idle;
            _lastKernelTime = kernel;
            _lastUserTime = user;
            _hasCpuBaseline = true;
            return CpuLoadPercent;
        }

        var idleDelta = idle - _lastIdleTime;
        var kernelDelta = kernel - _lastKernelTime;
        var userDelta = user - _lastUserTime;
        _lastIdleTime = idle;
        _lastKernelTime = kernel;
        _lastUserTime = user;

        var total = kernelDelta + userDelta;
        if (total == 0)
        {
            return CpuLoadPercent;
        }

        var used = total - idleDelta;
        return (int)Math.Clamp(Math.Round(used * 100d / total), 0, 100);
    }

    private static int SampleMemoryLoad()
    {
        var status = new MemoryStatusEx { Length = (uint)Marshal.SizeOf<MemoryStatusEx>() };
        return GlobalMemoryStatusEx(ref status)
            ? (int)Math.Clamp(status.MemoryLoad, 0, 100)
            : 0;
    }

    private static int SampleDiskLoad()
    {
        var systemDrive = Path.GetPathRoot(Environment.SystemDirectory) ?? "C:\\";
        if (!GetDiskFreeSpaceEx(systemDrive, out var freeBytes, out var totalBytes, out _) || totalBytes == 0)
        {
            return 0;
        }

        var usedBytes = totalBytes - freeBytes;
        return (int)Math.Clamp(Math.Round(usedBytes * 100d / totalBytes), 0, 100);
    }

    public void Dispose()
    {
        if (_disposed)
        {
            return;
        }

        _disposed = true;
        _hasCpuBaseline = false;
        ResourcesSampled = null;
        GC.SuppressFinalize(this);
    }

    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern bool GetSystemTimes(out ulong idleTime, out ulong kernelTime, out ulong userTime);

    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern bool GlobalMemoryStatusEx(ref MemoryStatusEx buffer);

    [DllImport("kernel32.dll", SetLastError = true, CharSet = CharSet.Unicode)]
    private static extern bool GetDiskFreeSpaceEx(
        string directoryName,
        out ulong freeBytesAvailable,
        out ulong totalNumberOfBytes,
        out ulong totalNumberOfFreeBytes);

    [StructLayout(LayoutKind.Sequential)]
    private struct MemoryStatusEx
    {
        public uint Length;
        public uint MemoryLoad;
        public ulong TotalPhysical;
        public ulong AvailablePhysical;
        public ulong TotalPageFile;
        public ulong AvailablePageFile;
        public ulong TotalVirtual;
        public ulong AvailableVirtual;
        public ulong AvailableExtendedVirtual;
    }
}