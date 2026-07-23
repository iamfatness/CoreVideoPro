# Deploy CoreVideo Pro staging Cloudflare workers (licensing, captions, telemetry, ops-monitor).
$ErrorActionPreference = "Stop"
$repoRoot = Resolve-Path (Join-Path $PSScriptRoot "..")

# Each service sets ONE secret from scripts/.staging-secrets.local before deploy.
# `Optional = $true` means the secret is skipped (not an error) when absent — the
# ops-monitor's alert webhook is optional (unset = log-only, beta S5).
$services = @(
  @{ Name = "licensing-api"; Secret = "LICENSE_API_KEY"; EnvVar = "COREVIDEO_LICENSE_API_KEY" },
  @{ Name = "caption-broker"; Secret = "CAPTION_BROKER_API_KEY"; EnvVar = "COREVIDEO_CAPTION_BROKER_API_KEY" },
  @{ Name = "telemetry-ingest"; Secret = "TELEMETRY_API_KEY"; EnvVar = "COREVIDEO_TELEMETRY_API_KEY" },
  @{ Name = "ops-monitor"; Secret = "OPS_ALERT_WEBHOOK_URL"; EnvVar = "COREVIDEO_OPS_ALERT_WEBHOOK_URL"; Optional = $true }
)

$secretsFile = Join-Path $PSScriptRoot ".staging-secrets.local"
if (-not (Test-Path $secretsFile)) {
  throw "Missing $secretsFile. Copy scripts/staging-services.env.example and set API keys before deploying."
}

foreach ($service in $services) {
  $serviceDir = Join-Path $repoRoot "services\$($service.Name)"
  if (-not (Test-Path $serviceDir)) {
    throw "Service directory not found: $serviceDir"
  }

  $secretValue = $null
  Get-Content $secretsFile | ForEach-Object {
    if ($_ -match "^$($service.EnvVar)=(.+)$") {
      $secretValue = $Matches[1].Trim()
    }
  }
  if (-not $secretValue -and -not $service.Optional) {
    throw "Missing $($service.EnvVar) in $secretsFile"
  }

  Write-Host "Deploying $($service.Name) ..."
  Push-Location $serviceDir
  try {
    if ($secretValue) {
      $secretValue | npx wrangler secret put $service.Secret
    } else {
      Write-Host "  (optional secret $($service.EnvVar) not set; deploying without it)"
    }
    npx wrangler deploy
  } finally {
    Pop-Location
  }
}

Write-Host "Staging workers deployed. Run: npm run smoke:staging-services"