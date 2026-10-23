# Loads staging Cloudflare worker endpoints + API keys into the current shell.
# STAGING OVERRIDE ONLY. Shipped builds bake the production endpoints
# (https://telemetry.corevideo.io, https://ops.corevideo.io, OAuth broker
# https://auth.corevideo.io) and need no env vars for them (#434/#436); dot-sourcing this
# script points THIS shell at the workers.dev staging Workers instead. The API
# key still comes from .staging-secrets.local (never baked, never committed).
# Usage: . .\scripts\load-staging-env.ps1

$ErrorActionPreference = "Stop"
$repoRoot = Resolve-Path (Join-Path $PSScriptRoot "..")
$secretsFile = Join-Path $PSScriptRoot ".staging-secrets.local"
$exampleFile = Join-Path $PSScriptRoot "staging-services.env.example"

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
Write-Host ("  TELEMETRY: " + $env:COREVIDEO_TELEMETRY_ENDPOINT)
Write-Host ("  OPS-MONITOR: " + $env:COREVIDEO_OPS_MONITOR_ENDPOINT)
