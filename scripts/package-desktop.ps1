# Package CoreVideo Pro for beta distribution (Windows-first).
# Optional signing: set CSC_LINK and CSC_KEY_PASSWORD before running.
param(
  [string]$NativeBuildDir = $(if ($env:COREVIDEO_NATIVE_BUILD_DIR) { $env:COREVIDEO_NATIVE_BUILD_DIR } else { (Join-Path $PSScriptRoot "..\native\build-dev\Release") }),
  [string]$PackTarget = $(if ($env:COREVIDEO_PACK_TARGET) { $env:COREVIDEO_PACK_TARGET } else { "--win" })
)

$ErrorActionPreference = "Stop"
$repoRoot = Join-Path $PSScriptRoot ".."

if (-not (Get-Command npm -ErrorAction SilentlyContinue)) {
  throw "npm is required on PATH."
}

if (-not (Test-Path (Join-Path $repoRoot "node_modules\.bin\electron-builder"))) {
  Write-Host "Installing packaging deps (electron + electron-builder)…" -ForegroundColor Yellow
  npm install --no-save electron@latest electron-builder@latest --prefix $repoRoot
}

$env:COREVIDEO_NATIVE_BUILD_DIR = $NativeBuildDir
$env:COREVIDEO_PACK_TARGET = $PackTarget

Push-Location $repoRoot
try {
  npm run pack:desktop
} finally {
  Pop-Location
}

Write-Host ""
Write-Host "Artifacts:" -ForegroundColor Green
Write-Host "  $repoRoot\artifacts\desktop"