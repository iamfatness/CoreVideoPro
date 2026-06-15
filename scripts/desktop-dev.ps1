# Launch CoreVideo Pro desktop shell with live Vite HMR.
# Prerequisites (one-time):
#   npm install
#   npm install --no-save electron@latest tsx
param(
  [int]$Port = 5174
)

$ErrorActionPreference = "Stop"
$repoRoot = Resolve-Path (Join-Path $PSScriptRoot "..")

Get-Process electron -ErrorAction SilentlyContinue | Stop-Process -Force

$electronExe = Join-Path $repoRoot "node_modules\electron\dist\electron.exe"
if (-not (Test-Path $electronExe)) {
  throw "Electron not installed. Run: npm install --no-save electron@latest tsx"
}

$tsxLoader = Join-Path $repoRoot "node_modules\tsx\dist\loader.mjs"
if (-not (Test-Path $tsxLoader)) {
  throw "tsx not installed. Run: npm install --no-save tsx"
}

$rendererUrl = "http://127.0.0.1:$Port"
Write-Host "Starting Vite on $rendererUrl ..."
$vite = Start-Process -FilePath "npm" -ArgumentList @("run", "dev", "--", "--port", "$Port", "--strictPort") `
  -WorkingDirectory $repoRoot -PassThru -WindowStyle Hidden

try {
  $ready = $false
  for ($attempt = 0; $attempt -lt 40; $attempt++) {
    try {
      $response = Invoke-WebRequest -Uri $rendererUrl -UseBasicParsing -TimeoutSec 1
      if ($response.StatusCode -eq 200) {
        $ready = $true
        break
      }
    } catch {
      Start-Sleep -Milliseconds 250
    }
  }

  if (-not $ready) {
    throw "Vite did not become ready at $rendererUrl"
  }

  Write-Host "Renderer ready. Starting Electron..."
  $env:COREVIDEO_RENDERER_URL = $rendererUrl
  Push-Location $repoRoot
  npm run desktop
  if ($LASTEXITCODE -ne 0) {
    exit $LASTEXITCODE
  }
} finally {
  if ($vite -and -not $vite.HasExited) {
    Stop-Process -Id $vite.Id -Force -ErrorAction SilentlyContinue
  }
  Pop-Location -ErrorAction SilentlyContinue
}