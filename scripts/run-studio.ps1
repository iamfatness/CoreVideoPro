param(
  [string]$Config = "Debug",
  [switch]$NoBuild,
  [switch]$UseDevNativeCore
)

$ErrorActionPreference = "Stop"
$repoRoot = Split-Path -Parent $PSScriptRoot
$nativeExe = Join-Path $repoRoot "native\build\corevideo-native.exe"
$studioExe = Join-Path $repoRoot "studio\build-clean\$Config\CoreVideoStudio.exe"

if (-not $NoBuild -or -not (Test-Path $nativeExe) -or -not (Test-Path $studioExe)) {
  & (Join-Path $PSScriptRoot "build-studio.ps1") -Config $Config
  if ($LASTEXITCODE -ne 0) {
    exit $LASTEXITCODE
  }
}

if (-not (Test-Path $nativeExe)) {
  throw "Missing native media core at $nativeExe. Run .\scripts\build-studio.ps1 from the repository root."
}
if (-not (Test-Path $studioExe)) {
  throw "Missing native Studio shell at $studioExe. Run .\scripts\build-studio.ps1 from the repository root."
}

if ($UseDevNativeCore) {
  $devNativeExe = Join-Path $repoRoot "native\build-dev\corevideo-native.exe"
  $devZoomEngine = Join-Path $repoRoot "native\build-dev\corevideo-zoom-engine.exe"
  if (-not (Test-Path $devNativeExe) -or -not (Test-Path $devZoomEngine)) {
    throw "Missing dev native Zoom build. Run .\scripts\build-native-dev.ps1 first."
  }
  $env:COREVIDEO_STUDIO_USE_DEV_CORE = "1"
  # Respect a pre-set engine path so the SYNTHETIC multi-participant engine
  # (native\build-dev\corevideo-zoom-engine-fake.exe) can be used to test the
  # multiview/program/preview paths WITHOUT a real Zoom meeting:
  #   $env:COREVIDEO_ZOOM_ENGINE_PATH = "...\corevideo-zoom-engine-fake.exe"; npm run app
  if (-not $env:COREVIDEO_ZOOM_ENGINE_PATH) {
    $env:COREVIDEO_ZOOM_ENGINE_PATH = $devZoomEngine
  }
  Write-Host "[run-studio] using dev native core: $devNativeExe" -ForegroundColor Cyan
  Write-Host "[run-studio] using Zoom engine: $env:COREVIDEO_ZOOM_ENGINE_PATH" -ForegroundColor Cyan
}

Write-Host "[run-studio] launching $studioExe" -ForegroundColor Cyan
Write-Host "[run-studio] working directory: $repoRoot" -ForegroundColor DarkGray
Start-Process -FilePath $studioExe -WorkingDirectory $repoRoot
