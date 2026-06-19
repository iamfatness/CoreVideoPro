param(
  [string]$Config = "Debug",
  [switch]$NoBuild
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

Write-Host "[run-studio] launching $studioExe" -ForegroundColor Cyan
Write-Host "[run-studio] working directory: $repoRoot" -ForegroundColor DarkGray
Start-Process -FilePath $studioExe -WorkingDirectory $repoRoot
