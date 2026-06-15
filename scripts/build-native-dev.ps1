# Build CoreVideo Pro native binaries for Sprint 1–3 (Zoom engine + output adapters).
# Requires: Visual Studio 2022+ with C++ tools, CMake, Zoom Meeting SDK x64.
param(
  [string]$ZoomSdkDir = $(if ($env:ZOOM_SDK_DIR) { $env:ZOOM_SDK_DIR } else { "C:\Users\walla\Downloads\zoom-sdk-windows-7.0.5.39292\zoom-sdk-windows-7.0.5.39292\x64" }),
  [string]$NativeDir = (Join-Path $PSScriptRoot "..\native"),
  [string]$BuildDir = (Join-Path $PSScriptRoot "..\native\build-dev")
)

$ErrorActionPreference = "Stop"
$vsDevCmd = "C:\Program Files\Microsoft Visual Studio\18\Community\Common7\Tools\VsDevCmd.bat"
if (-not (Test-Path $vsDevCmd)) {
  throw "Visual Studio Developer Command Prompt not found at: $vsDevCmd"
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
  "-DZOOM_SDK_DIR=$ZoomSdkDir"
)

$buildCmd = @(
  "call `"$vsDevCmd`" -arch=amd64",
  "cmake $($cmakeArgs -join ' ')",
  "cmake --build `"$BuildDir`" --config Release --target corevideo-zoom-engine corevideo-native corevideo-native-tests"
) -join " && "

cmd /c $buildCmd
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

Write-Host ""
Write-Host "Built:" -ForegroundColor Green
Write-Host "  $BuildDir\corevideo-zoom-engine.exe"
Write-Host "  $BuildDir\corevideo-native.exe"
Write-Host ""
Write-Host "Output adapters: D3D11 compositor, Media Foundation MP4, RTMP send-proof"
Write-Host "Next: .\scripts\sprint1-dev.ps1  (or .\scripts\sprint3-dev.ps1)"