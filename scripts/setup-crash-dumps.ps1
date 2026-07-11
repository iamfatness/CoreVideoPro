<#
.SYNOPSIS
  Configure Windows Error Reporting (WER) to capture FULL crash dumps for the
  CoreVideo Pro processes, so a native crash is diagnosable after the fact.

.DESCRIPTION
  By default WER LocalDumps (if enabled at all) writes a MINI dump — registers and
  stack only. That is what we had on 2026-07-10: the corevideo-native.exe startup
  crash gave a stack with no locals/heap, and once the binary was rebuilt even the
  offsets stopped resolving. This script registers a per-executable LocalDumps key
  with DumpType=2 (a full memory dump) for each CoreVideo Pro process, so the next
  crash yields a dump you can open in WinDbg/cdb with the co-located PDBs.

  Pair this with the Release PDBs now emitted by native/CMakeLists.txt and staged
  beside every binary (build-native-dev.ps1 / launch-native.ps1). Together:
  full dump + matching PDB = a real post-mortem.

  Writes under HKLM, so it requires elevation. If not already elevated it relaunches
  itself with a single UAC prompt. Idempotent — safe to re-run.

.NOTES
  Registry:  HKLM\SOFTWARE\Microsoft\Windows\Windows Error Reporting\LocalDumps\<exe>
  Dumps land in %LOCALAPPDATA%\CrashDumps (per-user, matches where they already go).
  To analyze:  cdb -y "<build-dev dir>;srv*https://msdl.microsoft.com/download/symbols" \
                   -z "%LOCALAPPDATA%\CrashDumps\<exe>.<pid>.dmp" -c "!analyze -v; q"
#>
param(
  [int]$DumpCount = 20,
  [string]$DumpFolder = '%LOCALAPPDATA%\CrashDumps'
)

$ErrorActionPreference = 'Stop'

$isAdmin = ([Security.Principal.WindowsPrincipal] `
    [Security.Principal.WindowsIdentity]::GetCurrent() `
  ).IsInRole([Security.Principal.WindowsBuiltinRole]::Administrator)

if (-not $isAdmin) {
  Write-Host "[setup-crash-dumps] elevation required (writes HKLM) — relaunching..." -ForegroundColor Yellow
  $psi = @{
    FilePath     = 'powershell.exe'
    Verb         = 'RunAs'
    ArgumentList = @(
      '-NoProfile', '-ExecutionPolicy', 'Bypass',
      '-File', "`"$PSCommandPath`"",
      '-DumpCount', $DumpCount,
      '-DumpFolder', "`"$DumpFolder`""
    )
  }
  Start-Process @psi
  return
}

# WER LocalDumps DumpType: 0=custom, 1=mini, 2=FULL. We want 2 for real post-mortems.
$FullDump = 2

$root = 'HKLM:\SOFTWARE\Microsoft\Windows\Windows Error Reporting\LocalDumps'
$exes = @(
  'corevideo-native.exe',       # the C++ media core — where native crashes happen
  'CoreVideoPro.WinUI.exe',     # the WinUI shell
  'corevideo-zoom-engine.exe'   # the Zoom SDK subprocess
)

if (-not (Test-Path $root)) {
  New-Item -Path $root -Force | Out-Null
  Write-Host "[setup-crash-dumps] created LocalDumps root" -ForegroundColor DarkGray
}

foreach ($exe in $exes) {
  $key = Join-Path $root $exe
  if (-not (Test-Path $key)) {
    New-Item -Path $key -Force | Out-Null
  }
  New-ItemProperty -Path $key -Name 'DumpType'   -PropertyType DWord      -Value $FullDump  -Force | Out-Null
  New-ItemProperty -Path $key -Name 'DumpCount'  -PropertyType DWord      -Value $DumpCount -Force | Out-Null
  New-ItemProperty -Path $key -Name 'DumpFolder' -PropertyType ExpandString -Value $DumpFolder -Force | Out-Null
  Write-Host "[setup-crash-dumps] $exe -> FULL dump, keep $DumpCount, folder $DumpFolder" -ForegroundColor Green
}

Write-Host ""
Write-Host "Done. Full crash dumps for the next crash land in $DumpFolder." -ForegroundColor Cyan
Write-Host "Analyze with:  cdb -y `"<native\build-dev>;srv*https://msdl.microsoft.com/download/symbols`" -z `"<dump>`" -c `"!analyze -v; q`"" -ForegroundColor DarkGray
