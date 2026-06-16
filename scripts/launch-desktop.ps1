# Launch CoreVideo Pro as a standalone desktop app (no Vite dev server).
# Builds the UI once, then opens the Electron window from local files.
$ErrorActionPreference = "Stop"
$repoRoot = Resolve-Path (Join-Path $PSScriptRoot "..")
Push-Location $repoRoot

$electronExe = Join-Path $repoRoot "node_modules\electron\dist\electron.exe"
if (-not (Test-Path $electronExe)) {
  Write-Host "Installing Electron (one-time)..."
  npm install --no-save electron@latest tsx
  node node_modules\electron\install.js
}

if (-not (Test-Path (Join-Path $repoRoot "node_modules\tsx\dist\loader.mjs"))) {
  npm install --no-save tsx
}

Remove-Item Env:COREVIDEO_RENDERER_URL -ErrorAction SilentlyContinue
npm run desktop
exit $LASTEXITCODE