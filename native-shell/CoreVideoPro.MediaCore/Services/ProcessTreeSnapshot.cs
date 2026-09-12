using System.Diagnostics;
using System.Runtime.InteropServices;

namespace CoreVideoPro.MediaCore.Services;

/// <summary>One descendant of a process, identified by pid AND start time (a pid alone can be
/// reused by an unrelated process after the original exits).</summary>
public readonly record struct DescendantProcess(int ProcessId, DateTime StartTimeUtc, string Name)
{
    public override string ToString() => $"pid {ProcessId} ({Name})";
}

/// <summary>
/// T1.8 (#461): the app-exit core stop lets the core exit on its own, and after a CLEAN exit
/// <c>Kill(entireProcessTree)</c> can no longer reach the core's children (their parent is gone).
/// Most children die with the core (destructors terminate them; RTMP/SRT/Zoom engine run in
/// kill-on-close jobs), but not every owner was verified, so the supervisor snapshots the core's
/// descendants before closing stdin and kills any that survive. Windows only; elsewhere it is a
/// no-op (the core does not ship there through this supervisor).
/// </summary>
public static class ProcessTreeSnapshot
{
    /// <summary>Every live descendant of <paramref name="root"/>, or empty if it cannot be read.</summary>
    public static IReadOnlyList<DescendantProcess> TryCaptureDescendants(Process root)
    {
        if (!OperatingSystem.IsWindows())
        {
            return [];
        }

        try
        {
            var rootStart = root.StartTime.ToUniversalTime();
            var parents = ReadParentMap();
            var result = new List<DescendantProcess>();
            var frontier = new Queue<(int Pid, DateTime Start)>();
            frontier.Enqueue((root.Id, rootStart));
            var seen = new HashSet<int> { root.Id };
            while (frontier.Count > 0)
            {
                var (parentPid, parentStart) = frontier.Dequeue();
                foreach (var (pid, parent) in parents)
                {
                    if (parent != parentPid || !seen.Add(pid)) continue;
                    if (!TryDescribe(pid, out var child)) continue;
                    // A process older than its "parent" is an orphan whose parent pid was reused.
                    if (child.StartTimeUtc < parentStart) continue;
                    result.Add(child);
                    frontier.Enqueue((pid, child.StartTimeUtc));
                }
            }

            return result;
        }
        catch
        {
            return [];
        }
    }

    /// <summary>Kills (tree) every recorded descendant still alive with the SAME start time, and
    /// returns what it killed.</summary>
    public static IReadOnlyList<DescendantProcess> KillSurvivors(IReadOnlyList<DescendantProcess> descendants)
    {
        var killed = new List<DescendantProcess>();
        foreach (var descendant in descendants)
        {
            try
            {
                using var process = Process.GetProcessById(descendant.ProcessId);
                if (process.HasExited || process.StartTime.ToUniversalTime() != descendant.StartTimeUtc) continue;
                process.Kill(entireProcessTree: true);
                killed.Add(descendant);
            }
            catch
            {
                // Gone already, or not ours to kill.
            }
        }

        return killed;
    }

    private static bool TryDescribe(int pid, out DescendantProcess descendant)
    {
        try
        {
            using var process = Process.GetProcessById(pid);
            descendant = new DescendantProcess(pid, process.StartTime.ToUniversalTime(), process.ProcessName);
            return true;
        }
        catch
        {
            descendant = default;
            return false;
        }
    }

    private static List<(int Pid, int ParentPid)> ReadParentMap()
    {
        var map = new List<(int, int)>();
        var snapshot = CreateToolhelp32Snapshot(Th32csSnapProcess, 0);
        if (snapshot == InvalidHandleValue)
        {
            return map;
        }

        try
        {
            var entry = new ProcessEntry32W { dwSize = (uint)Marshal.SizeOf<ProcessEntry32W>() };
            if (!Process32FirstW(snapshot, ref entry)) return map;
            do
            {
                map.Add(((int)entry.th32ProcessID, (int)entry.th32ParentProcessID));
            }
            while (Process32NextW(snapshot, ref entry));
        }
        finally
        {
            CloseHandle(snapshot);
        }

        return map;
    }

    private const uint Th32csSnapProcess = 0x00000002;
    private static readonly IntPtr InvalidHandleValue = new(-1);

    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
    private struct ProcessEntry32W
    {
        public uint dwSize;
        public uint cntUsage;
        public uint th32ProcessID;
        public IntPtr th32DefaultHeapID;
        public uint th32ModuleID;
        public uint cntThreads;
        public uint th32ParentProcessID;
        public int pcPriClassBase;
        public uint dwFlags;
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 260)]
        public string szExeFile;
    }

    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern IntPtr CreateToolhelp32Snapshot(uint dwFlags, uint th32ProcessID);

    [DllImport("kernel32.dll", SetLastError = true, CharSet = CharSet.Unicode)]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool Process32FirstW(IntPtr hSnapshot, ref ProcessEntry32W lppe);

    [DllImport("kernel32.dll", SetLastError = true, CharSet = CharSet.Unicode)]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool Process32NextW(IntPtr hSnapshot, ref ProcessEntry32W lppe);

    [DllImport("kernel32.dll", SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool CloseHandle(IntPtr hObject);
}
