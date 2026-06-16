# Stage Zoom Meeting SDK runtime files for the WinUI native-shell join path.
# Requires a locally installed SDK; this script does not download credentials or archives.
param(
  [string]$ZoomSdkDir = $(if ($env:ZOOM_SDK_DIR) { $env:ZOOM_SDK_DIR } else { "" }),
  [string]$RuntimeDir = $(if ($env:COREVIDEO_ZOOM_RUNTIME_DIR) { $env:COREVIDEO_ZOOM_RUNTIME_DIR } else { "" }),
  [ValidateSet("x64", "arm64")]
  [string]$Architecture = "x64",
  [switch]$Force
)

$ErrorActionPreference = "Stop"
$repoRoot = Split-Path -Parent $PSScriptRoot

$requiredFiles = @(
  @{ Id = "sdk-dll"; RelativePath = "bin\sdk.dll"; Purpose = "Runtime DLL loaded by the helper process." },
  @{ Id = "sdk-lib"; RelativePath = "lib\sdk.lib"; Purpose = "Import library used by the Windows helper build." },
  @{ Id = "zoom-sdk-header"; RelativePath = "h\zoom_sdk.h"; Purpose = "SDK initialization header." },
  @{ Id = "meeting-service-header"; RelativePath = "h\meeting_service_interface.h"; Purpose = "Meeting join/status service header." },
  @{ Id = "raw-video-api-header"; RelativePath = "h\rawdata\zoom_rawdata_api.h"; Purpose = "Raw media API entry points." },
  @{ Id = "raw-video-renderer-header"; RelativePath = "h\rawdata\rawdata_renderer_interface.h"; Purpose = "Raw participant video and share renderer callbacks." },
  @{ Id = "raw-audio-header"; RelativePath = "h\rawdata\rawdata_audio_helper_interface.h"; Purpose = "Raw mixed and per-participant audio callbacks." }
)

function Resolve-ArchitectureRoot([string]$SdkDir) {
  if ([string]::IsNullOrWhiteSpace($SdkDir)) {
    return $null
  }

  $normalized = [System.IO.Path]::GetFullPath($SdkDir)
  if (Test-Path (Join-Path $normalized "h\zoom_sdk.h")) {
    return $normalized
  }

  $archRoot = Join-Path $normalized $Architecture
  if (Test-Path (Join-Path $archRoot "h\zoom_sdk.h")) {
    return $archRoot
  }

  return $null
}

function Resolve-RuntimeTarget([string]$Override) {
  if (-not [string]::IsNullOrWhiteSpace($Override)) {
    return [System.IO.Path]::GetFullPath($Override)
  }

  return [System.IO.Path]::GetFullPath((Join-Path $repoRoot "native-core\zoom-runtime\windows\$Architecture"))
}

function Test-ZoomSdkPackage([string]$ArchitectureRoot) {
  $blockers = @()
  $warnings = @()

  if (-not (Test-Path $ArchitectureRoot)) {
    $blockers += "SDK architecture root folder was not found."
    return @{
      Status = "blocked"
      Blockers = $blockers
      Warnings = $warnings
      RuntimeDllCount = 0
      RuntimeExeCount = 0
      RawMediaHeaderCount = 0
    }
  }

  foreach ($file in $requiredFiles) {
    $fullPath = Join-Path $ArchitectureRoot $file.RelativePath
    if (-not (Test-Path $fullPath)) {
      $blockers += "$Architecture/$($file.RelativePath.Replace('\', '/')) is missing."
    }
  }

  $binDir = Join-Path $ArchitectureRoot "bin"
  $rawDataDir = Join-Path $ArchitectureRoot "h\rawdata"
  $runtimeDllCount = if (Test-Path $binDir) {
    (Get-ChildItem -Path $binDir -Filter "*.dll" -File -ErrorAction SilentlyContinue).Count
  } else { 0 }
  $runtimeExeCount = if (Test-Path $binDir) {
    (Get-ChildItem -Path $binDir -Filter "*.exe" -File -ErrorAction SilentlyContinue).Count
  } else { 0 }
  $rawMediaHeaderCount = if (Test-Path $rawDataDir) {
    (Get-ChildItem -Path $rawDataDir -Filter "*.h" -File -ErrorAction SilentlyContinue).Count
  } else { 0 }

  if ($runtimeDllCount -lt 20) {
    $warnings += "Only $runtimeDllCount runtime DLLs found; packaged helper may be missing Zoom dependencies."
  }
  if ($runtimeExeCount -eq 0) {
    $warnings += "No runtime EXE helpers found; zTscoder/crash/helper tools may be missing."
  }
  if ($rawMediaHeaderCount -lt 3) {
    $warnings += "Raw media headers are incomplete."
  }

  return @{
    Status = if ($blockers.Count -eq 0) { "ready" } else { "blocked" }
    Blockers = $blockers
    Warnings = $warnings
    RuntimeDllCount = $runtimeDllCount
    RuntimeExeCount = $runtimeExeCount
    RawMediaHeaderCount = $rawMediaHeaderCount
  }
}

function Show-StagingInstructions {
  Write-Host ""
  Write-Host "Zoom Meeting SDK staging skipped (SDK not configured)." -ForegroundColor Yellow
  Write-Host ""
  Write-Host "Discovery env vars:" -ForegroundColor Cyan
  Write-Host "  ZOOM_SDK_DIR                 Source SDK x64 folder or package root"
  Write-Host "  COREVIDEO_ZOOM_RUNTIME_DIR   Override staged runtime target folder"
  Write-Host "  COREVIDEO_ZOOM_JWT_BROKER_URL Optional dev JWT broker URL for Settings readiness"
  Write-Host ""
  Write-Host "To stage locally:" -ForegroundColor Cyan
  Write-Host "  1. Download the Zoom Meeting SDK for Windows from the Zoom developer portal."
  Write-Host "  2. Extract the archive and set ZOOM_SDK_DIR to the x64 folder (or package root)."
  Write-Host "  3. Run: .\scripts\stage-zoom-sdk.ps1"
  Write-Host ""
  Write-Host "Default runtime target:" -ForegroundColor Cyan
  Write-Host "  native-core\zoom-runtime\windows\$Architecture"
  Write-Host ""
  Write-Host "After staging, build native helpers with .\scripts\build-native-dev.ps1" -ForegroundColor DarkGray
}

$architectureRoot = Resolve-ArchitectureRoot -SdkDir $ZoomSdkDir
$runtimeTarget = Resolve-RuntimeTarget -Override $RuntimeDir

if (-not $architectureRoot) {
  Show-StagingInstructions
  exit 0
}

$sourceReport = Test-ZoomSdkPackage -ArchitectureRoot $architectureRoot
if ($sourceReport.Status -ne "ready") {
  Write-Host "Zoom SDK source at $architectureRoot is incomplete:" -ForegroundColor Red
  foreach ($blocker in $sourceReport.Blockers) {
    Write-Host "  - $blocker" -ForegroundColor Red
  }
  exit 1
}

if ((Test-Path $runtimeTarget) -and -not $Force) {
  $existingReport = Test-ZoomSdkPackage -ArchitectureRoot $runtimeTarget
  if ($existingReport.Status -eq "ready") {
    Write-Host "Zoom SDK runtime already staged at $runtimeTarget" -ForegroundColor Green
    foreach ($warning in $existingReport.Warnings) {
      Write-Host "  warning: $warning" -ForegroundColor Yellow
    }
    exit 0
  }
}

Write-Host "[stage:zoom] copying SDK from $architectureRoot" -ForegroundColor Cyan
Write-Host "[stage:zoom] target runtime: $runtimeTarget" -ForegroundColor Cyan

if (Test-Path $runtimeTarget) {
  Remove-Item $runtimeTarget -Recurse -Force
}
New-Item -ItemType Directory -Path $runtimeTarget -Force | Out-Null

$binSource = Join-Path $architectureRoot "bin"
$libSource = Join-Path $architectureRoot "lib"
$headerSource = Join-Path $architectureRoot "h"
$binTarget = Join-Path $runtimeTarget "bin"
$libTarget = Join-Path $runtimeTarget "lib"
$headerTarget = Join-Path $runtimeTarget "h"

New-Item -ItemType Directory -Path $binTarget -Force | Out-Null
New-Item -ItemType Directory -Path $libTarget -Force | Out-Null
New-Item -ItemType Directory -Path $headerTarget -Force | Out-Null

Copy-Item -Path (Join-Path $binSource "*") -Destination $binTarget -Recurse -Force
Copy-Item -Path (Join-Path $libSource "sdk.lib") -Destination (Join-Path $libTarget "sdk.lib") -Force
Copy-Item -Path (Join-Path $headerSource "*") -Destination $headerTarget -Recurse -Force

$stagedReport = Test-ZoomSdkPackage -ArchitectureRoot $runtimeTarget
if ($stagedReport.Status -ne "ready") {
  Write-Host "Staged Zoom SDK runtime failed validation:" -ForegroundColor Red
  foreach ($blocker in $stagedReport.Blockers) {
    Write-Host "  - $blocker" -ForegroundColor Red
  }
  exit 1
}

Write-Host ""
Write-Host "Zoom SDK runtime staged:" -ForegroundColor Green
Write-Host "  $runtimeTarget"
Write-Host "  $($stagedReport.RuntimeDllCount) DLLs, $($stagedReport.RuntimeExeCount) helper EXEs, $($stagedReport.RawMediaHeaderCount) raw-media headers"
foreach ($warning in $stagedReport.Warnings) {
  Write-Host "  warning: $warning" -ForegroundColor Yellow
}
Write-Host ""
Write-Host "Next:" -ForegroundColor Cyan
Write-Host "  .\scripts\build-native-dev.ps1"
Write-Host "  npm run test:native-shell"