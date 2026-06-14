# Sprint 1 dev launcher: Electron + native media core + vendored Zoom engine.
# Prerequisites:
#   1. npm install
#   2. npm install --no-save electron@latest
#   3. .\scripts\build-native-dev.ps1
#   4. Set COREVIDEO_ZOOM_PUBLIC_APP_KEY (or COREVIDEO_ZOOM_SDK_JWT) in your environment
#   5. Optional: COREVIDEO_ZOOM_MEETING_PASSCODE, join tokens
param(
  [string]$BuildDir = (Join-Path $PSScriptRoot "..\native\build-dev"),
  [string]$MeetingUrl = "",
  [switch]$SkipRendererBuild
)

$ErrorActionPreference = "Stop"
$repoRoot = Resolve-Path (Join-Path $PSScriptRoot "..")

$nativeCore = Join-Path $BuildDir "corevideo-native.exe"
$zoomEngine = Join-Path $BuildDir "corevideo-zoom-engine.exe"

foreach ($path in @($nativeCore, $zoomEngine)) {
  if (-not (Test-Path $path)) {
    throw "Missing $path — run .\scripts\build-native-dev.ps1 first."
  }
}

if (-not $env:COREVIDEO_ZOOM_PUBLIC_APP_KEY -and -not $env:COREVIDEO_ZOOM_SDK_JWT) {
  Write-Warning "Set COREVIDEO_ZOOM_PUBLIC_APP_KEY or COREVIDEO_ZOOM_SDK_JWT before joining a real meeting."
}

$env:COREVIDEO_MEDIA_CORE_COMMAND = $nativeCore
$env:COREVIDEO_ZOOM_ENGINE_PATH = $zoomEngine
$env:COREVIDEO_RENDERER_URL = "http://127.0.0.1:5173"

if ($MeetingUrl) {
  $env:COREVIDEO_DEFAULT_MEETING_URL = $MeetingUrl
}

Write-Host "Native core : $nativeCore"
Write-Host "Zoom engine : $zoomEngine"
Write-Host "Renderer    : $($env:COREVIDEO_RENDERER_URL)"
Write-Host ""

$vite = Start-Process -FilePath "npm" -ArgumentList @("run", "dev") -WorkingDirectory $repoRoot -PassThru -WindowStyle Hidden
Start-Sleep -Seconds 3

try {
  if (-not $SkipRendererBuild) {
    Push-Location $repoRoot
    npm run desktop
    Pop-Location
  } else {
    Push-Location $repoRoot
    node desktop/scripts/launch.mjs
    Pop-Location
  }
} finally {
  if (-not $vite.HasExited) {
    Stop-Process -Id $vite.Id -Force -ErrorAction SilentlyContinue
  }
}