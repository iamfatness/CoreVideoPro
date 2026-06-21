<#
.SYNOPSIS
  One command to build the native media core (best available tier) and launch the
  CoreVideo Pro WinUI app.

.DESCRIPTION
  The WinUI shell is a thin UI over a native C++ media core (corevideo-native.exe).
  "Launching" only feels complicated because building that core is a separate step
  with capability tiers. This script does the whole thing in one shot: it picks the
  best core it can build on this machine, builds it (only when missing or stale),
  then builds + launches the app and tells you exactly which tier you got.

    full  - real Zoom capture, D3D11 compositor, recording, RTMP, and audio.
            Requires the Zoom SDK (auto-detected via ZOOM_SDK_DIR or .\ZoomSDK).
    audio - real Windows audio capture + monitor; video / Zoom / streaming are
            synthetic. No Zoom SDK required.
    stub  - everything synthetic. Only used if no native core can be built.

  By default an existing core is reused and rebuilt only when the native sources are
  newer. Use -Rebuild to force a rebuild, or -StubOnly to skip the core build.

.EXAMPLE
  npm run app
.EXAMPLE
  npm run app -- -Rebuild
#>
param(
  [switch]$Rebuild,
  [switch]$StubOnly
)

$ErrorActionPreference = "Stop"
$repoRoot = Split-Path -Parent $PSScriptRoot

function Find-NativeCore {
  foreach ($rel in @(
      "native\build-dev\corevideo-native.exe",
      "native\build-dev\Release\corevideo-native.exe",
      "native\build\corevideo-native.exe",
      "native\build\Release\corevideo-native.exe")) {
    $path = Join-Path $repoRoot $rel
    if (Test-Path $path) {
      return (Get-Item $path)
    }
  }
  return $null
}

function Get-CoreTier($coreItem) {
  if (-not $coreItem) { return "stub" }
  if ($coreItem.FullName -match "build-dev") { return "full" }
  return "audio"
}

function Test-CoreStale($coreItem) {
  if (-not $coreItem) { return $true }
  $newest = $null
  foreach ($rel in @("native\src", "native\CMakeLists.txt")) {
    $root = Join-Path $repoRoot $rel
    if (-not (Test-Path $root)) { continue }
    $latest = Get-ChildItem -Path $root -Recurse -File -ErrorAction SilentlyContinue |
      Sort-Object LastWriteTimeUtc -Descending | Select-Object -First 1
    if ($latest -and (($null -eq $newest) -or ($latest.LastWriteTimeUtc -gt $newest))) {
      $newest = $latest.LastWriteTimeUtc
    }
  }
  if ($null -eq $newest) { return $false }
  return ($coreItem.LastWriteTimeUtc -lt $newest)
}

function Test-ZoomSdkAvailable {
  if ($env:ZOOM_SDK_DIR -and (Test-Path (Join-Path $env:ZOOM_SDK_DIR "h\zoom_sdk.h"))) {
    return $true
  }
  $sdkRoot = Join-Path $repoRoot "ZoomSDK"
  if (-not (Test-Path $sdkRoot)) { return $false }
  return [bool](Get-ChildItem -Path $sdkRoot -Recurse -Filter "zoom_sdk.h" -ErrorAction SilentlyContinue |
    Select-Object -First 1)
}

$core = Find-NativeCore
$needBuild = (-not $StubOnly) -and ($Rebuild -or (-not $core) -or (Test-CoreStale $core))

if ($StubOnly) {
  Write-Host "[app] -StubOnly set; skipping the native core build." -ForegroundColor Yellow
}

if ($needBuild) {
  $built = $false

  if (Test-ZoomSdkAvailable) {
    Write-Host "[app] Zoom SDK detected - building the FULL native core (Zoom + D3D11 + RTMP + audio)..." -ForegroundColor Cyan
    & powershell.exe -NoProfile -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot "build-native-dev.ps1")
    if ($LASTEXITCODE -eq 0) {
      $built = $true
    } else {
      Write-Host "[app] Full dev build did not complete; falling back to the audio-capable core." -ForegroundColor Yellow
    }
  } else {
    Write-Host "[app] No Zoom SDK found - building the AUDIO-capable core (real Windows audio, synthetic video)." -ForegroundColor Yellow
    Write-Host "[app] (Set ZOOM_SDK_DIR or stage .\ZoomSDK to get the full Zoom + streaming core.)" -ForegroundColor DarkGray
  }

  if (-not $built) {
    & powershell.exe -NoProfile -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot "build-studio.ps1") -Config Release
    if ($LASTEXITCODE -eq 0) {
      $built = $true
    } else {
      Write-Host "[app] Native core build failed - launching with a STUB core (everything synthetic)." -ForegroundColor Red
      Write-Host "[app] Install Visual Studio 2022+ (Desktop C++ workload) + CMake, then run: npm run app -- -Rebuild" -ForegroundColor Yellow
    }
  }

  $core = Find-NativeCore
}

$tier = Get-CoreTier $core
switch ($tier) {
  "full"  { $tierMsg = "FULL  - real Zoom capture, GPU compositor, recording, RTMP, and audio."; $tierColor = "Green"; break }
  "audio" { $tierMsg = "AUDIO - real Windows audio (capture + monitor); video / Zoom / streaming are synthetic."; $tierColor = "Green"; break }
  default { $tierMsg = "STUB  - everything synthetic (no native core built)."; $tierColor = "Yellow" }
}

Write-Host ""
Write-Host "[app] Media core tier: $tier" -ForegroundColor $tierColor
Write-Host "[app] $tierMsg"
if ($core) {
  Write-Host "[app] Using core: $($core.FullName)" -ForegroundColor DarkGray
}
Write-Host ""
Write-Host "[app] Building + launching the WinUI app..." -ForegroundColor Cyan

& powershell.exe -NoProfile -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot "launch-native.ps1")
exit $LASTEXITCODE
