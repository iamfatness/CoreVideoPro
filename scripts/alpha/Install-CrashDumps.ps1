# T2.1 / #433. Enables WER LocalDumps (full dumps) for the three CoreVideo Pro
# executables. Windows reads LocalDumps from HKLM only (an HKCU key produces zero
# dumps; measured 2026-09-12), so this is the installer's elevated step.
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
    [switch]$Elevated,   # internal: this is the RunAs child
    # Overridable so tests can exercise the real logic against a throwaway HKCU key without elevation.
    [string]$RegistryRoot = 'HKLM:\SOFTWARE\Microsoft\Windows\Windows Error Reporting\LocalDumps',
    [string]$UninstallRoot = 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Uninstall',
    [string]$DumpFolder = '%LOCALAPPDATA%\CoreVideoPro\crashes',
    [string]$LogPath = ''
)
$ErrorActionPreference = 'Stop'
$executables = @('CoreVideoPro.WinUI.exe', 'corevideo-native.exe', 'corevideo-zoom-engine.exe')
$ownerValue = 'CoreVideoProInstaller'   # marks a key this installer wrote, so we never touch one we did not
$dumpType = 2   # full
$dumpCount = 5

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

function Test-Managed([string]$key) {
    if (-not (Test-Path -LiteralPath $key)) { return $false }
    return $null -ne (Get-ItemProperty -LiteralPath $key -Name $ownerValue -ErrorAction SilentlyContinue)
}

# Another registered CoreVideo Pro version still relies on these keys; the exe names are shared across versions.
function Test-OtherInstallRegistered {
    if (-not (Test-Path -LiteralPath $UninstallRoot)) { return $false }
    foreach ($k in Get-ChildItem -LiteralPath $UninstallRoot -ErrorAction SilentlyContinue) {
        if ($k.PSChildName -notlike 'CoreVideoPro-*') { continue }
        if ($ReleaseId -and $k.PSChildName -eq "CoreVideoPro-$ReleaseId") { continue }
        $loc = (Get-ItemProperty -LiteralPath $k.PSPath -ErrorAction SilentlyContinue).InstallLocation
        if ($loc -and (Test-Path -LiteralPath $loc)) { return $true }
    }
    return $false
}

function Invoke-Machine {
    foreach ($exe in $executables) {
        $key = Join-Path $RegistryRoot $exe
        switch ($Action) {
            'Install' {
                if ((Test-Path -LiteralPath $key) -and -not (Test-Managed $key)) {
                    # Someone else (e.g. scripts/setup-crash-dumps.ps1) configured this exe; do not clobber it.
                    Write-Log "PRESERVED existing LocalDumps configuration for $exe (not written by this installer)."
                    continue
                }
                New-Item -Path $key -Force | Out-Null
                New-ItemProperty -LiteralPath $key -Name DumpFolder -PropertyType ExpandString -Value $DumpFolder -Force | Out-Null
                New-ItemProperty -LiteralPath $key -Name DumpType -PropertyType DWord -Value $dumpType -Force | Out-Null
                New-ItemProperty -LiteralPath $key -Name DumpCount -PropertyType DWord -Value $dumpCount -Force | Out-Null
                New-ItemProperty -LiteralPath $key -Name $ownerValue -PropertyType String -Value 'CoreVideo Pro' -Force | Out-Null
                Write-Log "Enabled full crash dumps for $exe -> $DumpFolder (keep $dumpCount)."
            }
            'Uninstall' {
                if (-not (Test-Path -LiteralPath $key)) { continue }
                if (-not (Test-Managed $key)) { Write-Log "Left $exe LocalDumps key alone (not written by this installer)."; continue }
                Remove-Item -LiteralPath $key -Recurse -Force
                Write-Log "Removed LocalDumps key for $exe."
            }
        }
    }
}

try {
    if ($Action -eq 'Inspect') {
        $state = foreach ($exe in $executables) {
            $key = Join-Path $RegistryRoot $exe
            $p = if (Test-Path -LiteralPath $key) { Get-ItemProperty -LiteralPath $key } else { $null }
            [ordered]@{ exe = $exe; present = [bool]$p; managed = (Test-Managed $key)
                dumpFolder = $p.DumpFolder; dumpType = $p.DumpType; dumpCount = $p.DumpCount }
        }
        $state | ConvertTo-Json -Depth 4
        exit 0
    }
    if ($Action -eq 'Uninstall' -and (Test-OtherInstallRegistered)) {
        Write-Log 'Kept LocalDumps keys: another CoreVideo Pro version is still installed.'
        exit 0
    }
    $needsAdmin = $RegistryRoot -like 'HKLM:*'
    if ($needsAdmin -and -not (Test-Admin)) {
        if (-not $AllowElevation) {
            Write-Log 'WARNING: crash dumps NOT configured: administrator approval is required to write HKLM LocalDumps and this run is not elevated (silent setup must already be elevated). Run scripts\setup-crash-dumps.ps1 elevated to enable them.'
            exit 2
        }
        $argText = '-NoProfile -ExecutionPolicy Bypass -File "{0}" -Action {1} -Elevated -ReleaseId "{2}" -DumpFolder "{3}"' -f $PSCommandPath, $Action, $ReleaseId, $DumpFolder
        if ($LogPath) { $argText += (' -LogPath "{0}"' -f $LogPath) }
        try {
            $child = Start-Process powershell.exe -Verb RunAs -WindowStyle Hidden -ArgumentList $argText -PassThru -Wait
        } catch {
            Write-Log "WARNING: crash dumps NOT configured: administrator approval was declined or unavailable ($($_.Exception.Message))."
            exit 2
        }
        if ($child.ExitCode -ne 0) { Write-Log "WARNING: crash dumps NOT configured: elevated step exited $($child.ExitCode)."; exit 2 }
        Write-Log "Crash dump configuration $Action completed (elevated)."
        exit 0
    }
    Invoke-Machine
    exit 0
} catch {
    Write-Log "WARNING: crash dumps NOT configured: $($_.Exception.Message)"
    exit 2
}
