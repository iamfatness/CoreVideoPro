# Hang watchdog: the moment CoreVideoPro.WinUI stops responding, capture FULL
# dumps of the shell AND the core BEFORE the operator reaches Task Manager.
#
# Why this exists (2026-08-09): "turning off one zoom source" froze the shell
# and the core in the SAME second. Both logs just stop; a Task Manager kill
# leaves no dump, so the hang class is undiagnosable after the fact. A hang
# dump shows every thread's wait chain - one capture ends the guessing.
#
#   powershell -File tools\hang-watchdog.ps1        # runs until Ctrl+C
#
# Dumps land in %LOCALAPPDATA%\CoreVideoPro\hang-dumps\ (full-memory, via the
# same MiniDumpWriteDump WER uses). The watchdog captures ONCE per hang episode
# and re-arms when the app responds again or restarts.
$ErrorActionPreference = "Continue"
$outDir = Join-Path $env:LOCALAPPDATA "CoreVideoPro\hang-dumps"
New-Item -ItemType Directory -Force $outDir | Out-Null

$signature = @'
using System;
using System.Diagnostics;
using System.IO;
using System.Runtime.InteropServices;
public static class Dumper {
    [DllImport("dbghelp.dll", SetLastError = true)]
    static extern bool MiniDumpWriteDump(IntPtr hProcess, uint processId, SafeHandle hFile,
        int dumpType, IntPtr exceptionParam, IntPtr userStreamParam, IntPtr callbackParam);
    // MiniDumpWithFullMemory | WithHandleData | WithThreadInfo | WithUnloadedModules
    const int FullDump = 0x2 | 0x4 | 0x1000 | 0x20;
    public static bool Write(Process p, string path) {
        using (var fs = new FileStream(path, FileMode.Create, FileAccess.ReadWrite, FileShare.None))
            return MiniDumpWriteDump(p.Handle, (uint)p.Id, fs.SafeFileHandle, FullDump,
                                     IntPtr.Zero, IntPtr.Zero, IntPtr.Zero);
    }
}
'@
Add-Type -TypeDefinition $signature

Write-Host "[watchdog] armed - watching CoreVideoPro.WinUI for hangs (dumps -> $outDir)"
$capturedForPid = 0
while ($true) {
    Start-Sleep -Seconds 2
    $app = Get-Process CoreVideoPro.WinUI -ErrorAction SilentlyContinue | Select-Object -First 1
    if (-not $app) { $capturedForPid = 0; continue }
    $app.Refresh()
    if ($app.Responding) { if ($capturedForPid -eq $app.Id) { $capturedForPid = 0; Write-Host "[watchdog] app responsive again - re-armed" }; continue }

    # Not responding: confirm it holds for 5s (menu-open etc. can blip).
    Start-Sleep -Seconds 5
    $app.Refresh()
    if ($app.Responding -or $capturedForPid -eq $app.Id) { continue }

    $stamp = Get-Date -Format "yyyyMMdd-HHmmss"
    Write-Host "[watchdog] HANG detected (pid $($app.Id)) - capturing dumps..."
    foreach ($proc in @($app) + @(Get-Process corevideo-native, corevideo-zoom-engine -ErrorAction SilentlyContinue)) {
        $path = Join-Path $outDir "$($proc.ProcessName)-$($proc.Id)-$stamp.dmp"
        try {
            $ok = [Dumper]::Write($proc, $path)
            Write-Host "  $($proc.ProcessName) pid=$($proc.Id) -> $(if ($ok) { "OK $([math]::Round((Get-Item $path).Length/1MB))MB" } else { "FAILED" })"
        } catch { Write-Host "  $($proc.ProcessName): $($_.Exception.Message)" }
    }
    $capturedForPid = $app.Id
    Write-Host "[watchdog] capture complete - leave the app as-is or kill it; dumps are safe."
}
