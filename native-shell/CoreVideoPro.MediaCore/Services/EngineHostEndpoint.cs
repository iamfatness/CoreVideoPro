using System.Diagnostics;
using System.IO.Pipes;
using System.Runtime.InteropServices;
using System.Security.Cryptography;
using System.Security.Principal;
using System.Text;

namespace CoreVideoPro.MediaCore.Services;

public static class EngineHostEndpoint
{
    public static string Name(string profile)
    {
        if (!OperatingSystem.IsWindows()) throw new PlatformNotSupportedException("EngineHost is Windows-only.");
        var identity = WindowsIdentity.GetCurrent().User?.Value ?? throw new InvalidOperationException("Missing user SID.");
        var hash = SHA256.HashData(Encoding.UTF8.GetBytes(identity + "|" + Process.GetCurrentProcess().SessionId + "|" + profile));
        return "CoreVideo.EngineHost." + Convert.ToHexString(hash)[..32];
    }

    public static void VerifyServer(NamedPipeClientStream pipe, string expectedExecutable)
    {
        if (!GetNamedPipeServerProcessId(pipe.SafePipeHandle.DangerousGetHandle(), out var pid))
            throw new IOException("Cannot verify engine host identity.");
        using var process = Process.GetProcessById(checked((int)pid));
        if (process.SessionId != Process.GetCurrentProcess().SessionId ||
            !string.Equals(Path.GetFullPath(process.MainModule!.FileName), Path.GetFullPath(expectedExecutable), StringComparison.OrdinalIgnoreCase))
            throw new IOException("Engine host belongs to another session or installation.");
    }

    public static void VerifyClient(NamedPipeServerStream pipe)
    {
        var machine = new StringBuilder(256);
        var hasComputerName = GetNamedPipeClientComputerName(pipe.SafePipeHandle.DangerousGetHandle(), machine, (uint)machine.Capacity);
        // Windows reports ERROR_PIPE_LOCAL (229) for a local-only endpoint.
        if (!hasComputerName && Marshal.GetLastWin32Error() != 229)
            throw new IOException("Cannot verify local client machine.");
        if (hasComputerName)
            throw new IOException("Remote named-pipe clients are not accepted.");
        if (!GetNamedPipeClientProcessId(pipe.SafePipeHandle.DangerousGetHandle(), out var pid))
            throw new IOException("Cannot query client PID: " + Marshal.GetLastWin32Error());
        using var process = Process.GetProcessById(checked((int)pid));
        if (process.SessionId != Process.GetCurrentProcess().SessionId) throw new IOException("Client session differs.");
    }

    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern bool GetNamedPipeServerProcessId(IntPtr pipe, out uint pid);
    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern bool GetNamedPipeClientProcessId(IntPtr pipe, out uint pid);
    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    private static extern bool GetNamedPipeClientComputerName(IntPtr pipe, StringBuilder name, uint size);
}
