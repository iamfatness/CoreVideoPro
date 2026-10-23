# T2.1 / #433. Enables WER LocalDumps (full dumps) for the three CoreVideo Pro
# executables. Windows reads LocalDumps from HKLM only (an HKCU key produces zero
# dumps; measured 2026-09-12), so this is the installer's elevated step.
#
# 64-BIT VIEW IS MANDATORY. The installer is a 32-bit NSIS binary, so the
# PowerShell it launches is the 32-bit SysWOW64 host, and the HKLM: PSDrive
# follows the PROCESS view: it would write HKLM\SOFTWARE\WOW6432Node\..., which
# WerFault never reads for 64-bit processes (zero dumps). Every registry access
# here goes through OpenBaseKey(..., Registry64), exactly as Install-VirtualCamera.ps1
# and VirtualCameraRegistration.psm1 do. Never use HKLM: / New-Item / Set-ItemProperty here.
#
# NEVER fatal to setup: any refusal (UAC declined, key write denied) is logged
# loudly and reported through the exit code (0 = configured/preserved,
# 2 = not configured), and the installer carries on. Without -AllowElevation a
# non-admin run does not prompt (silent setup must already be elevated).
[CmdletBinding()]
param(
    [ValidateSet('Install','Uninstall','Inspect')][string]$Action = 'Install',
    [string]$ReleaseId = '',
    [switch]$AllowElevation,
    # Hive/paths are overridable so tests can run the real logic against a throwaway HKCU key without elevation.
    [ValidateSet('LocalMachine','CurrentUser')][string]$Hive = 'LocalMachine',
    [string]$LocalDumpsPath = 'SOFTWARE\Microsoft\Windows\Windows Error Reporting\LocalDumps',
    [string]$UninstallPath = 'Software\Microsoft\Windows\CurrentVersion\Uninstall',
    [string]$DumpFolder = '%LOCALAPPDATA%\CoreVideoPro\crashes',
    [string]$LogPath = ''
)
$ErrorActionPreference = 'Stop'
$executables = @('CoreVideoPro.WinUI.exe', 'corevideo-native.exe', 'corevideo-zoom-engine.exe')
$ownerValue = 'CoreVideoProInstaller'   # marks a key this installer wrote, so we never touch one we did not
$dumpType = 2   # full
$dumpCount = 5
$view = [Microsoft.Win32.RegistryView]::Registry64

function Write-Log([string]$message) {
    Write-Output $message
    if ($LogPath) {
        try {
            New-Item -ItemType Directory -Force -Path (Split-Path -Parent $LogPath) | Out-Null
            Add-Content -LiteralPath $LogPath -Value ('{0:o} {1}' -f (Get-Date), $message)
        } catch { }
    }
}

function Test-Admin {
    $p = [Security.Principal.WindowsPrincipal]::new([Security.Principal.WindowsIdentity]::GetCurrent())
    return $p.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
}

function Open-Base([string]$hiveName) {
    return [Microsoft.Win32.RegistryKey]::OpenBaseKey([Microsoft.Win32.RegistryHive]::$hiveName, $view)
}

# Reads one executable's key from the 64-bit view; $null when absent.
function Read-Key([string]$exe) {
    $base = Open-Base $Hive
    try {
        $k = $base.OpenSubKey("$LocalDumpsPath\$exe")
        if (-not $k) { return $null }
        try {
            return [pscustomobject]@{
                Managed = ($null -ne $k.GetValue($ownerValue))
                DumpFolder = $k.GetValue('DumpFolder', $null, [Microsoft.Win32.RegistryValueOptions]::DoNotExpandEnvironmentNames)
                DumpFolderKind = $(if ($null -ne $k.GetValue('DumpFolder')) { $k.GetValueKind('DumpFolder').ToString() } else { $null })
                DumpType = $k.GetValue('DumpType')
                DumpCount = $k.GetValue('DumpCount')
            }
        } finally { $k.Dispose() }
    } finally { $base.Dispose() }
}

# Another registered CoreVideo Pro version still relies on these keys; the exe names are shared across versions.
function Test-OtherInstallRegistered {
    $base = Open-Base 'CurrentUser'
    try {
        $root = $base.OpenSubKey($UninstallPath)
        if (-not $root) { return $false }
        try {
            foreach ($name in $root.GetSubKeyNames()) {
                if ($name -notlike 'CoreVideoPro-*') { continue }
                if ($ReleaseId -and $name -eq "CoreVideoPro-$ReleaseId") { continue }
                $k = $root.OpenSubKey($name)
                if (-not $k) { continue }
                try { $loc = [string]$k.GetValue('InstallLocation') } finally { $k.Dispose() }
                if ($loc -and (Test-Path -LiteralPath $loc)) { return $true }
            }
        } finally { $root.Dispose() }
    } finally { $base.Dispose() }
    return $false
}

function Invoke-Machine {
    $base = Open-Base $Hive
    try {
        foreach ($exe in $executables) {
            $path = "$LocalDumpsPath\$exe"
            $existing = Read-Key $exe
            if ($Action -eq 'Install') {
                if ($existing -and -not $existing.Managed) {
                    # Someone else (e.g. scripts/setup-crash-dumps.ps1) configured this exe; do not clobber it.
                    Write-Log "PRESERVED existing LocalDumps configuration for $exe (not written by this installer)."
                    continue
                }
                $k = $base.CreateSubKey($path)
                try {
                    $k.SetValue('DumpFolder', $DumpFolder, [Microsoft.Win32.RegistryValueKind]::ExpandString)
                    $k.SetValue('DumpType', $dumpType, [Microsoft.Win32.RegistryValueKind]::DWord)
                    $k.SetValue('DumpCount', $dumpCount, [Microsoft.Win32.RegistryValueKind]::DWord)
                    $k.SetValue($ownerValue, 'CoreVideo Pro', [Microsoft.Win32.RegistryValueKind]::String)
                } finally { $k.Dispose() }
                Write-Log "Enabled full crash dumps for $exe -> $DumpFolder (keep $dumpCount)."
            } else {
                if (-not $existing) { continue }
                if (-not $existing.Managed) { Write-Log "Left $exe LocalDumps key alone (not written by this installer)."; continue }
                $base.DeleteSubKeyTree($path)
                Write-Log "Removed LocalDumps key for $exe."
            }
        }
    } finally { $base.Dispose() }
}

# True when the 64-bit view shows the requested end state.
function Test-EndState {
    foreach ($exe in $executables) {
        $s = Read-Key $exe
        if ($Action -eq 'Install') {
            if (-not $s) { return $false }
            if ($s.Managed -and ($s.DumpType -ne $dumpType -or $s.DumpCount -ne $dumpCount -or $s.DumpFolder -ne $DumpFolder)) { return $false }
        } elseif ($s -and $s.Managed) { return $false }
    }
    return $true
}

try {
    if ($Action -eq 'Inspect') {
        $state = foreach ($exe in $executables) {
            $s = Read-Key $exe
            [ordered]@{ exe = $exe; present = [bool]$s; managed = [bool]($s -and $s.Managed)
                dumpFolder = $s.DumpFolder; dumpType = $s.DumpType; dumpCount = $s.DumpCount }
        }
        $state | ConvertTo-Json -Depth 4
        exit 0
    }
    if ($Action -eq 'Uninstall' -and (Test-OtherInstallRegistered)) {
        Write-Log 'Kept LocalDumps keys: another CoreVideo Pro version is still installed.'
        exit 0
    }
    if ($Hive -eq 'LocalMachine' -and -not (Test-Admin)) {
        if (-not $AllowElevation) {
            Write-Log 'WARNING: crash dumps NOT configured: administrator approval is required to write HKLM LocalDumps and this run is not elevated (silent setup must already be elevated). Run scripts\setup-crash-dumps.ps1 elevated to enable them.'
            exit 2
        }
        # Re-launch THIS host elevated (the registry view is explicit, so its bitness does not matter).
        $argText = '-NoProfile -ExecutionPolicy Bypass -File "{0}" -Action {1} -ReleaseId "{2}" -DumpFolder "{3}"' -f $PSCommandPath, $Action, $ReleaseId, $DumpFolder
        if ($LogPath) { $argText += (' -LogPath "{0}"' -f $LogPath) }
        $hostExe = Join-Path $PSHOME 'powershell.exe'
        try {
            $child = Start-Process $hostExe -Verb RunAs -WindowStyle Hidden -ArgumentList $argText -PassThru -Wait
        } catch {
            Write-Log "WARNING: crash dumps NOT configured: administrator approval was declined or unavailable ($($_.Exception.Message))."
            exit 2
        }
        $code = $child.ExitCode
        if ($null -eq $code) {
            # Some systems do not report an elevated child's exit code: unknown, not failure. Trust the registry.
            if (Test-EndState) { Write-Log "Crash dump configuration $Action completed (elevated; exit code unavailable, verified by reading the 64-bit registry view)."; exit 0 }
            Write-Log 'WARNING: crash dumps NOT configured: elevated step reported no exit code and the 64-bit registry view does not show the expected state.'
            exit 2
        }
        if ($code -ne 0) { Write-Log "WARNING: crash dumps NOT configured: elevated step exited $code."; exit 2 }
        Write-Log "Crash dump configuration $Action completed (elevated)."
        exit 0
    }
    Invoke-Machine
    exit 0
} catch {
    Write-Log "WARNING: crash dumps NOT configured: $($_.Exception.Message)"
    exit 2
}
