# Launch CoreVideo Pro desktop shell with live Vite HMR.
# Prerequisites (one-time):
#   npm install
#   npm install --no-save electron@latest tsx
param(
  [int]$Port = 0
)

$ErrorActionPreference = "Stop"
$repoRoot = Resolve-Path (Join-Path $PSScriptRoot "..")

function Stop-ListenerOnPort {
  param([int]$ListenPort)
  $procIds = Get-NetTCPConnection -LocalPort $ListenPort -State Listen -ErrorAction SilentlyContinue |
    Select-Object -ExpandProperty OwningProcess -Unique
  foreach ($procId in $procIds) {
    if ($procId) {
      Stop-Process -Id $procId -Force -ErrorAction SilentlyContinue
    }
  }
}

function Test-RendererReady {
  param([string]$Url)
  try {
    $response = Invoke-WebRequest -Uri $Url -UseBasicParsing -TimeoutSec 1
    return $response.StatusCode -eq 200
  } catch {
    return $false
  }
}

function Find-FreePort {
  param([int]$StartPort, [int]$EndPort)
  for ($candidate = $StartPort; $candidate -le $EndPort; $candidate++) {
    $inUse = Get-NetTCPConnection -LocalPort $candidate -State Listen -ErrorAction SilentlyContinue
    if (-not $inUse) {
      return $candidate
    }
  }
  throw "No free port between $StartPort and $EndPort. Run: npm run stop:desktop"
}

# Clean up previous dev session for this repo.
& (Join-Path $PSScriptRoot "stop-desktop-dev.ps1") | Out-Null
Start-Sleep -Milliseconds 400

$electronExe = Join-Path $repoRoot "node_modules\electron\dist\electron.exe"
if (-not (Test-Path $electronExe)) {
  Write-Host "Electron binary missing. Installing..."
  Push-Location $repoRoot
  npm install --no-save electron@latest tsx
  node node_modules\electron\install.js
  Pop-Location
  if (-not (Test-Path $electronExe)) {
    throw "Electron not installed. Run: npm install --no-save electron@latest tsx"
  }
}

$tsxLoader = Join-Path $repoRoot "node_modules\tsx\dist\loader.mjs"
if (-not (Test-Path $tsxLoader)) {
  throw "tsx not installed. Run: npm install --no-save tsx"
}

$viteEntry = Join-Path $repoRoot "node_modules\vite\bin\vite.js"
if (-not (Test-Path $viteEntry)) {
  throw "Vite not installed. Run: npm install"
}

if ($Port -le 0) {
  $Port = Find-FreePort -StartPort 5174 -EndPort 5190
}

$rendererUrl = "http://127.0.0.1:$Port"
Write-Host "Starting Vite on $rendererUrl ..."

$vite = Start-Process -FilePath "node" `
  -ArgumentList @($viteEntry, "--host", "127.0.0.1", "--port", "$Port", "--strictPort") `
  -WorkingDirectory $repoRoot `
  -PassThru `
  -WindowStyle Hidden

$exitCode = 0
try {
  $ready = $false
  for ($attempt = 0; $attempt -lt 80; $attempt++) {
    if (Test-RendererReady -Url $rendererUrl) {
      $ready = $true
      break
    }
    if ($vite.HasExited) {
      throw "Vite exited before becoming ready (code $($vite.ExitCode)). Check $repoRoot for errors."
    }
    Start-Sleep -Milliseconds 250
  }

  if (-not $ready) {
    throw "Vite did not become ready at $rendererUrl"
  }

  Write-Host "Renderer ready. Starting Electron..."
  $env:COREVIDEO_RENDERER_URL = $rendererUrl
  Push-Location $repoRoot
  npm run desktop
  $exitCode = $LASTEXITCODE
  if ($exitCode -ne 0 -and $exitCode -ne $null) {
    Write-Host "Electron exited with code $exitCode (this is normal after closing the window if code is 0)."
  }
} catch {
  Write-Host "ERROR: $($_.Exception.Message)" -ForegroundColor Red
  $exitCode = 1
} finally {
  if ($vite -and -not $vite.HasExited) {
    Stop-Process -Id $vite.Id -Force -ErrorAction SilentlyContinue
  }
  Pop-Location -ErrorAction SilentlyContinue
}

exit $exitCode