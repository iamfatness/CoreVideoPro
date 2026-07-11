<#
.SYNOPSIS
  Configure Windows Error Reporting (WER) to capture FULL crash dumps for the
  CoreVideo Pro processes, so a native crash is diagnosable after the fact.

.DESCRIPTION
  By default WER LocalDumps (if enabled at all) writes a MINI dump - registers and
  stack only. That is what we had on 2026-07-10: the corevideo-native.exe startup
  crash gave a stack with no locals/heap, and once the binary was rebuilt even the
  offsets stopped resolving. This script registers a per-executable LocalDumps key
  with DumpType=2 (a full memory dump) for each CoreVideo Pro process, so the next
  crash yields a dump you can open in WinDbg/cdb with the co-located PDBs.

  Pair this with the Release PDBs now emitted by native/CMakeLists.txt and staged
  beside every binary (build-native-dev.ps1 / launch-native.ps1). Together:
  full dump + matching PDB = a real post-mortem.

  Writes under HKLM, so it requires elevation. If not already elevated it relaunches
  itself with a single UAC prompt. Idempotent - safe to re-run.

.NOTES
  Registry:  HKLM\SOFTWARE\Microsoft\Windows\Windows Error Reporting\LocalDumps\<exe>
  Dumps land in %LOCALAPPDATA%\CrashDumps (per-user, matches where they already go).
#>
param(
  [int]$DumpCount = 20,
  [string]$DumpFolder = '%LOCALAPPDATA%\CrashDumps'
)

$ErrorActionPreference = 'Stop'

$identity = [Security.Principal.WindowsIdentity]::GetCurrent()
$principal = New-Object Security.Principal.WindowsPrincipal($identity)
$isAdmin = $principal.IsInRole([Security.Principal.WindowsBuiltinRole]::Administrator)

if (-not $isAdmin) {
  Write-Host '[setup-crash-dumps] elevation required (writes HKLM) - relaunching...' -ForegroundColor Yellow
  $argList = @(
    '-NoProfile', '-ExecutionPolicy', 'Bypass',
    '-File', ('"{0}"' -f $PSCommandPath),
    '-DumpCount', $DumpCount,
    '-DumpFolder', ('"{0}"' -f $DumpFolder)
  )
  Start-Process -FilePath 'powershell.exe' -Verb RunAs -ArgumentList $argList
  return
}

# WER LocalDumps DumpType: 0=custom, 1=mini, 2=FULL. We want 2 for real post-mortems.
$FullDump = 2

$root = 'HKLM:\SOFTWARE\Microsoft\Windows\Windows Error Reporting\LocalDumps'
$exes = @(
  'corevideo-native.exe',
  'CoreVideoPro.WinUI.exe',
  'corevideo-zoom-engine.exe'
)

if (-not (Test-Path $root)) {
  New-Item -Path $root -Force | Out-Null
  Write-Host '[setup-crash-dumps] created LocalDumps root' -ForegroundColor DarkGray
}

foreach ($exe in $exes) {
  $key = Join-Path $root $exe
  if (-not (Test-Path $key)) {
    New-Item -Path $key -Force | Out-Null
  }
  New-ItemProperty -Path $key -Name 'DumpType'   -PropertyType DWord        -Value $FullDump  -Force | Out-Null
  New-ItemProperty -Path $key -Name 'DumpCount'  -PropertyType DWord        -Value $DumpCount -Force | Out-Null
  New-ItemProperty -Path $key -Name 'DumpFolder' -PropertyType ExpandString -Value $DumpFolder -Force | Out-Null
  Write-Host ("[setup-crash-dumps] {0} -> FULL dump, keep {1}, folder {2}" -f $exe, $DumpCount, $DumpFolder) -ForegroundColor Green
}

Write-Host ''
Write-Host ("Done. Full crash dumps for the next crash land in {0}." -f $DumpFolder) -ForegroundColor Cyan
