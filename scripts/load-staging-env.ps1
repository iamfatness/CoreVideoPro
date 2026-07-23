# Loads staging Cloudflare worker endpoints + API keys into the current shell.
# Usage: . .\scripts\load-staging-env.ps1

$ErrorActionPreference = "Stop"
$repoRoot = Resolve-Path (Join-Path $PSScriptRoot "..")
$secretsFile = Join-Path $PSScriptRoot ".staging-secrets.local"
$exampleFile = Join-Path $PSScriptRoot "staging-services.env.example"

$env:COREVIDEO_LICENSE_API_ENDPOINT = "https://corevideo-licensing-api.wallace-john-w.workers.dev"
$env:COREVIDEO_CAPTION_BROKER_ENDPOINT = "https://corevideo-caption-broker.wallace-john-w.workers.dev"
$env:COREVIDEO_TELEMETRY_ENDPOINT = "https://corevideo-telemetry-ingest.wallace-john-w.workers.dev"
$env:COREVIDEO_OPS_MONITOR_ENDPOINT = "https://corevideo-ops-monitor.wallace-john-w.workers.dev"

if (-not (Test-Path $secretsFile)) {
  Write-Warning "Missing $secretsFile - copy $exampleFile and add the Cloudflare worker API keys."
  return
}

Get-Content $secretsFile | ForEach-Object {
  $line = $_.Trim()
  if ($line.Length -eq 0 -or $line.StartsWith("#")) {
    return
  }
  $parts = $line -split "=", 2
  if ($parts.Length -eq 2) {
    $name = $parts[0].Trim()
    $value = $parts[1].Trim()
    if ($name.Length -gt 0) {
      Set-Item -Path "env:$name" -Value $value
    }
  }
}

Write-Host "Staging services loaded:"
Write-Host ("  LICENSE: " + $env:COREVIDEO_LICENSE_API_ENDPOINT)
Write-Host ("  CAPTIONS: " + $env:COREVIDEO_CAPTION_BROKER_ENDPOINT)
Write-Host ("  TELEMETRY: " + $env:COREVIDEO_TELEMETRY_ENDPOINT)
Write-Host ("  OPS-MONITOR: " + $env:COREVIDEO_OPS_MONITOR_ENDPOINT)