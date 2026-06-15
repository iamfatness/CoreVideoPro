# Smoke-test deployed staging workers (trial, captions, crash upload).
$ErrorActionPreference = "Stop"
. (Join-Path $PSScriptRoot "load-staging-env.ps1")

if (-not $env:COREVIDEO_LICENSE_API_KEY) {
  throw "COREVIDEO_LICENSE_API_KEY is not set. Check scripts/.staging-secrets.local"
}

$auth = @{
  Authorization = "Bearer $($env:COREVIDEO_LICENSE_API_KEY)"
  "content-type" = "application/json"
}

$trial = Invoke-RestMethod `
  -Uri "$($env:COREVIDEO_LICENSE_API_ENDPOINT)/v1/trial/start" `
  -Method POST `
  -Headers $auth `
  -Body '{"email":"smoke@corevideo.pro","deviceId":"smoke-device"}'
Write-Host "License trial: $($trial.licenseKey)"

$verify = Invoke-RestMethod `
  -Uri "$($env:COREVIDEO_LICENSE_API_ENDPOINT)/v1/license/verify?licenseKey=$($trial.licenseKey)&deviceId=smoke-device" `
  -Headers @{ Authorization = "Bearer $($env:COREVIDEO_LICENSE_API_KEY)" }
Write-Host "License verify: valid=$($verify.valid) tier=$($verify.entitlements.tier)"

$captionAuth = @{
  Authorization = "Bearer $($env:COREVIDEO_CAPTION_BROKER_API_KEY)"
  "content-type" = "application/json"
}
$session = Invoke-RestMethod `
  -Uri "$($env:COREVIDEO_CAPTION_BROKER_ENDPOINT)/v1/sessions" `
  -Method POST `
  -Headers $captionAuth `
  -Body '{"productionSessionId":"smoke-show"}'
$chunk = Invoke-RestMethod `
  -Uri "$($env:COREVIDEO_CAPTION_BROKER_ENDPOINT)/v1/sessions/$($session.brokerSessionId)/chunk" `
  -Method POST `
  -Headers $captionAuth `
  -Body '{"atMs":1200,"speakerName":"Maya Chen","audioLevel":55}'
Write-Host "Caption cue: $($chunk.cues[0].text)"

$telemetryAuth = @{
  Authorization = "Bearer $($env:COREVIDEO_TELEMETRY_API_KEY)"
  "content-type" = "application/json"
}
$crash = Invoke-RestMethod `
  -Uri "$($env:COREVIDEO_TELEMETRY_ENDPOINT)/v1/crashes" `
  -Method POST `
  -Headers $telemetryAuth `
  -Body '{"reason":"smoke-test","app":{"version":"0.1.0","platform":"win32"}}'
Write-Host "Telemetry report: $($crash.reportId)"

Write-Host "Staging smoke passed."