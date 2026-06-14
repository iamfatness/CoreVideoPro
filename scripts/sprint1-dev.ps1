# Sprint 1 dev launcher: Electron + native media core + vendored Zoom engine.
# Prerequisites:
#   1. npm install
#   2. npm install --no-save electron@latest
#   3. .\scripts\build-native-dev.ps1
#   4. Optional: scripts/sprint1-local.ps1 for JWT / join tokens (not the public app key)
param(
  [string]$BuildDir = (Join-Path $PSScriptRoot "..\native\build-dev"),
  [string]$MeetingUrl = "",
  [switch]$SkipRendererBuild
)

$ErrorActionPreference = "Stop"
$repoRoot = Resolve-Path (Join-Path $PSScriptRoot "..")

$embeddedKeyConfig = Join-Path $repoRoot "src\config\zoomMeetingSdk.json"
if (Test-Path $embeddedKeyConfig) {
  $sdkConfig = Get-Content $embeddedKeyConfig -Raw | ConvertFrom-Json
  if (-not $env:COREVIDEO_ZOOM_PUBLIC_APP_KEY) {
    $env:COREVIDEO_ZOOM_PUBLIC_APP_KEY = $sdkConfig.publicAppKey
  }
}

$localSecrets = Join-Path $PSScriptRoot "sprint1-local.ps1"
if (Test-Path $localSecrets) {
  . $localSecrets
  Write-Host "Loaded optional local overrides from scripts/sprint1-local.ps1"
}

$nativeCore = Join-Path $BuildDir "corevideo-native.exe"
$zoomEngine = Join-Path $BuildDir "corevideo-zoom-engine.exe"

foreach ($path in @($nativeCore, $zoomEngine)) {
  if (-not (Test-Path $path)) {
    throw "Missing $path — run .\scripts\build-native-dev.ps1 first."
  }
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
Write-Host "Zoom app key: embedded (src/config/zoomMeetingSdk.json)"
if ($env:COREVIDEO_DEFAULT_MEETING_URL) {
  Write-Host "Meeting URL : $($env:COREVIDEO_DEFAULT_MEETING_URL)"
}
if ($env:COREVIDEO_ZOOM_MEETING_PASSCODE) {
  Write-Host "Passcode    : configured (scripts/sprint1-local.ps1)"
}
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