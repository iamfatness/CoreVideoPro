# Build CoreVideo Pro native binaries for Sprint 1–3 (Zoom engine + output adapters).
# Requires: Visual Studio 2022+ with C++ tools, CMake, Zoom Meeting SDK x64.
param(
  [string]$ZoomSdkDir = "",
  [string]$NativeDir = (Join-Path $PSScriptRoot "..\native"),
  [string]$BuildDir = (Join-Path $PSScriptRoot "..\native\build-dev"),
  [string]$Config = "Release",
  [switch]$WithDeckLink,
  [switch]$WithAja
)

$ErrorActionPreference = "Stop"
$repoRoot = Split-Path -Parent $PSScriptRoot

function Resolve-DefaultZoomSdkDir {
  if ($env:ZOOM_SDK_DIR) {
    return $env:ZOOM_SDK_DIR
  }

  $repoSdkCandidates = @(
    (Join-Path $repoRoot "ZoomSDK\zoom-sdk-windows-7.0.5.39292\x64"),
    (Join-Path $repoRoot "ZoomSDK\x64")
  )
  foreach ($candidate in $repoSdkCandidates) {
    if (Test-Path (Join-Path $candidate "h\zoom_sdk.h")) {
      return $candidate
    }
  }

  return "C:\Users\walla\Downloads\zoom-sdk-windows-7.0.5.39292\zoom-sdk-windows-7.0.5.39292\x64"
}

if (-not $PSBoundParameters.ContainsKey("ZoomSdkDir")) {
  $ZoomSdkDir = Resolve-DefaultZoomSdkDir
}

function Resolve-VsDevCmd {
  $candidates = @(
    "${env:ProgramFiles}\Microsoft Visual Studio\2022\Community\Common7\Tools\VsDevCmd.bat",
    "${env:ProgramFiles}\Microsoft Visual Studio\2022\Professional\Common7\Tools\VsDevCmd.bat",
    "${env:ProgramFiles}\Microsoft Visual Studio\2022\Enterprise\Common7\Tools\VsDevCmd.bat",
    "${env:ProgramFiles}\Microsoft Visual Studio\18\Community\Common7\Tools\VsDevCmd.bat",
    "${env:ProgramFiles(x86)}\Microsoft Visual Studio\2022\BuildTools\Common7\Tools\VsDevCmd.bat"
  )
  return $candidates | Where-Object { Test-Path $_ } | Select-Object -First 1
}

function Quote-CmdArg([string]$Value) {
  if ($Value -match '[\s"]') {
    return '"' + ($Value -replace '"', '\"') + '"'
  }
  return $Value
}

function Stage-DevNativeArtifacts {
  param(
    [string]$SourceDir,
    [string]$Config
  )

  # CMake writes Release binaries to build-dev/ (CMAKE_RUNTIME_OUTPUT_DIRECTORY).
  # Stage copies to build-dev/Release so pack scripts and older probes stay aligned.
  $targets = @(
    $BuildDir,
    (Join-Path $BuildDir $Config)
  )

  foreach ($targetDir in $targets) {
    if (-not (Test-Path $targetDir)) {
      New-Item -ItemType Directory -Path $targetDir -Force | Out-Null
    }
    foreach ($name in @("corevideo-native.exe", "corevideo-native-tests.exe", "corevideo-zoom-engine.exe")) {
      $source = Join-Path $SourceDir $name
      $destination = Join-Path $targetDir $name
      if (-not (Test-Path $source)) {
        continue
      }
      $resolvedDestination = Resolve-Path $destination -ErrorAction SilentlyContinue
      if ($resolvedDestination -and (Resolve-Path $source).Path -ieq $resolvedDestination.Path) {
        continue
      }
      Copy-Item -Path $source -Destination $destination -Force
      Write-Host "[build-native-dev] staged $name -> $targetDir" -ForegroundColor DarkGray
    }
  }
}

$vsDevCmd = Resolve-VsDevCmd
if (-not $vsDevCmd) {
  throw "Visual Studio Developer Command Prompt not found. Install VS 2022+ or Build Tools with the C++ workload."
}
if (-not (Test-Path (Join-Path $ZoomSdkDir "h\zoom_sdk.h"))) {
  throw "Zoom SDK not found. Set -ZoomSdkDir or ZOOM_SDK_DIR to the x64 SDK folder (h/zoom_sdk.h + lib/sdk.lib)."
}

$cmakeArgs = @(
  "-S", $NativeDir,
  "-B", $BuildDir,
  "-DCOREVIDEO_STUB=OFF",
  "-DCOREVIDEO_ENABLE_DEV_ADAPTERS=ON",
  "-DCOREVIDEO_WITH_D3D11=ON",
  "-DCOREVIDEO_WITH_MF_ENCODER=ON",
  "-DCOREVIDEO_WITH_RTMP_OUTPUT=ON",
  "-DCOREVIDEO_BUILD_ZOOM_ENGINE=ON",
  "-DBUILD_TESTING=ON",
  "-DZOOM_SDK_DIR=$ZoomSdkDir"
)

if ($WithDeckLink) {
  $cmakeArgs += "-DCOREVIDEO_WITH_DECKLINK=ON"
}
if ($WithAja) {
  $cmakeArgs += "-DCOREVIDEO_WITH_AJA=ON"
}

$buildCmd = @(
  "call `"$vsDevCmd`" -arch=amd64",
  "cmake $(($cmakeArgs | ForEach-Object { Quote-CmdArg $_ }) -join ' ')",
  "cmake --build `"$BuildDir`" --config $Config --target corevideo-zoom-engine corevideo-native corevideo-native-tests"
) -join " && "

cmd /c $buildCmd
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

$artifactCandidates = @(
  $BuildDir,
  (Join-Path $BuildDir $Config)
)
$artifactDir = $artifactCandidates | Where-Object { Test-Path (Join-Path $_ "corevideo-native.exe") } | Select-Object -First 1
if (-not $artifactDir) {
  throw "corevideo-native.exe not found under $BuildDir"
}

Stage-DevNativeArtifacts -SourceDir $artifactDir -Config $Config

Write-Host ""
Write-Host "Built:" -ForegroundColor Green
Write-Host "  $BuildDir\corevideo-native.exe"
Write-Host "  $BuildDir\corevideo-zoom-engine.exe"
Write-Host "  $BuildDir\corevideo-native-tests.exe"
Write-Host ""
Write-Host "Output adapters: D3D11 compositor, Media Foundation MP4, RTMP send-proof"
if ($WithDeckLink) { Write-Host "Capture adapters: Blackmagic DeckLink enumeration enabled" }
if ($WithAja) { Write-Host "Capture adapters: AJA NTV2 runtime probe enabled" }
Write-Host "Verify: ctest -C $Config --test-dir `"$BuildDir`" --output-on-failure"
Write-Host "Next: .\scripts\sprint1-dev.ps1  (or .\scripts\sprint3-dev.ps1)"