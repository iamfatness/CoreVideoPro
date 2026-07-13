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
    (Join-Path $repoRoot "ZoomSDK\zoom-sdk-windows-7.0.5.39292\zoom-sdk-windows-7.0.5.39292\x64"),
    (Join-Path $repoRoot "ZoomSDK\zoom-sdk-windows-7.0.5.39292\x64"),
    (Join-Path $repoRoot "ZoomSDK\zoom-sdk-windows-7.0.5.39292\zoom-sdk-windows-7.0.5.39292"),
    (Join-Path $repoRoot "ZoomSDK\zoom-sdk-windows-7.0.5.39292"),
    (Join-Path $repoRoot "ZoomSDK\x64")
  )
  foreach ($candidate in $repoSdkCandidates) {
    if (Test-Path (Join-Path $candidate "h\zoom_sdk.h")) {
      return $candidate
    }
    $x64 = Join-Path $candidate "x64"
    if (Test-Path (Join-Path $x64 "h\zoom_sdk.h")) {
      return $x64
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
    "${env:ProgramFiles}\Microsoft Visual Studio\2026\Community\Common7\Tools\VsDevCmd.bat",
    "${env:ProgramFiles}\Microsoft Visual Studio\2026\Professional\Common7\Tools\VsDevCmd.bat",
    "${env:ProgramFiles}\Microsoft Visual Studio\2026\Enterprise\Common7\Tools\VsDevCmd.bat",
    "${env:ProgramFiles}\Microsoft Visual Studio\18\Community\Common7\Tools\VsDevCmd.bat",
    "${env:ProgramFiles}\Microsoft Visual Studio\18\Professional\Common7\Tools\VsDevCmd.bat",
    "${env:ProgramFiles}\Microsoft Visual Studio\18\Enterprise\Common7\Tools\VsDevCmd.bat",
    "${env:ProgramFiles(x86)}\Microsoft Visual Studio\2022\BuildTools\Common7\Tools\VsDevCmd.bat"
  )
  $candidate = $candidates | Where-Object { Test-Path $_ } | Select-Object -First 1
  if ($candidate) {
    return $candidate
  }

  $vswhere = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe"
  if (Test-Path $vswhere) {
    $discovered = & $vswhere -latest -products * -requires Microsoft.Component.MSBuild -find "Common7\Tools\VsDevCmd.bat" | Select-Object -First 1
    if ($discovered -and (Test-Path $discovered)) {
      return $discovered
    }
  }

  return $null
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
    foreach ($name in @("corevideo-native.exe", "corevideo-native-tests.exe", "corevideo-zoom-engine.exe", "corevideo-zoom-engine-fake.exe", "corevideo-plugin-host.exe", "corevideo-browser-host.exe")) {
      # Stage each binary AND its matching .pdb — the PDB is what makes a crash dump
      # of this exact build diagnosable (see native/CMakeLists.txt Release /DEBUG).
      foreach ($artifact in @($name, [System.IO.Path]::ChangeExtension($name, ".pdb"))) {
        $source = Join-Path $SourceDir $artifact
        $destination = Join-Path $targetDir $artifact
        if (-not (Test-Path $source)) {
          continue
        }
        $resolvedDestination = Resolve-Path $destination -ErrorAction SilentlyContinue
        if ($resolvedDestination -and (Resolve-Path $source).Path -ieq $resolvedDestination.Path) {
          continue
        }
        Copy-Item -Path $source -Destination $destination -Force
        Write-Host "[build-native-dev] staged $artifact -> $targetDir" -ForegroundColor DarkGray
      }
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

Write-Host "[build-native-dev] ensuring staged Zoom runtime..." -ForegroundColor Cyan
& (Join-Path $repoRoot "scripts\stage-zoom-sdk.ps1") -ZoomSdkDir $ZoomSdkDir
# stage-zoom-sdk.ps1 falls off the end (no `exit`) on a successful fresh stage, so
# $LASTEXITCODE can be $null in a fresh shell — only a REAL nonzero code is a failure
# (a bare `-ne 0` made the first build in a fresh worktree stop right here).
if ($null -ne $LASTEXITCODE -and $LASTEXITCODE -ne 0) {
  exit $LASTEXITCODE
}

$stagedZoomRuntime = Join-Path $repoRoot "native-core\zoom-runtime\windows\x64"
if (-not (Test-Path (Join-Path $stagedZoomRuntime "bin\sdk.dll"))) {
  throw "Staged Zoom runtime is missing at $stagedZoomRuntime after stage-zoom-sdk.ps1."
}

$cmakeArgs = @(
  "-S", $NativeDir,
  "-B", $BuildDir,
  "-DCOREVIDEO_STUB=OFF",
  "-DCOREVIDEO_ENABLE_DEV_ADAPTERS=ON",
  "-DCOREVIDEO_WITH_ZOOM=ON",
  "-DCOREVIDEO_ZOOM_SDK_ROOT=$stagedZoomRuntime",
  "-DCOREVIDEO_WITH_D3D11=ON",
  "-DCOREVIDEO_WITH_MF_ENCODER=ON",
  "-DCOREVIDEO_WITH_WASAPI_MONITOR=ON",
  "-DCOREVIDEO_WITH_WASAPI_CAPTURE=ON",
  "-DCOREVIDEO_WITH_RTMP_OUTPUT=ON",
  "-DCOREVIDEO_WITH_NDI_OUTPUT=ON",
  "-DCOREVIDEO_WITH_UVC=ON",
  "-DCOREVIDEO_WITH_WGC=ON",
  "-DCOREVIDEO_WITH_VIRTUALCAM=ON",
  "-DCOREVIDEO_WITH_BROWSER_HOST=ON",
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
  "cmake --build `"$BuildDir`" --config $Config --target corevideo-zoom-engine corevideo-zoom-engine-fake corevideo-plugin-host corevideo-native corevideo-native-tests corevideo-virtualcam corevideo-browser-host"
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
Write-Host "Output adapters: D3D11 compositor, Media Foundation MP4, RTMP send-proof, NDI runtime-probe"
if ($WithDeckLink) { Write-Host "Capture adapters: Blackmagic DeckLink enumeration enabled" }
if ($WithAja) { Write-Host "Capture adapters: AJA NTV2 runtime probe enabled" }
Write-Host "Verify: ctest -C $Config --test-dir `"$BuildDir`" --output-on-failure"
Write-Host "Next: .\scripts\sprint1-dev.ps1  (or .\scripts\sprint3-dev.ps1)"
