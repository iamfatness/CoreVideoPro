# Smoke-test the deployed staging telemetry-ingest worker (crash + event upload).
# licensing-api and caption-broker were removed in #743 (stub services with no
# native client); ops-monitor is probed by its own cron, not here.
$ErrorActionPreference = "Stop"
. (Join-Path $PSScriptRoot "load-staging-env.ps1")

if (-not $env:COREVIDEO_TELEMETRY_API_KEY) {
  throw "COREVIDEO_TELEMETRY_API_KEY is not set (telemetry-ingest requires auth since S0). Check scripts/.staging-secrets.local"
}

$telemetryAuth = @{
  Authorization = "Bearer $($env:COREVIDEO_TELEMETRY_API_KEY)"
  "content-type" = "application/json"
}
$crash = Invoke-RestMethod `
  -Uri "$($env:COREVIDEO_TELEMETRY_ENDPOINT)/v1/crashes" `
  -Method POST `
  -Headers $telemetryAuth `
  -Body '{"reason":"smoke-test","app":{"version":"0.1.0","platform":"win32"}}'
if (-not $crash.reportId) {
  throw "Telemetry crash upload returned no reportId"
}
Write-Host "Telemetry crash report: $($crash.reportId)"

$telemetryEvent = Invoke-RestMethod `
  -Uri "$($env:COREVIDEO_TELEMETRY_ENDPOINT)/v1/events" `
  -Method POST `
  -Headers $telemetryAuth `
  -Body '{"name":"smoke-event","app":{"version":"0.1.0","platform":"win32"}}'
if (-not $telemetryEvent.reportId) {
  throw "Telemetry event upload returned no reportId"
}
Write-Host "Telemetry event: $($telemetryEvent.reportId)"

Write-Host "Staging smoke passed."
