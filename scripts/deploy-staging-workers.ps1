# Deploy CoreVideo Pro staging Cloudflare workers (licensing, captions, telemetry).
$ErrorActionPreference = "Stop"
$repoRoot = Resolve-Path (Join-Path $PSScriptRoot "..")

$services = @(
  @{ Name = "licensing-api"; Secret = "LICENSE_API_KEY"; EnvVar = "COREVIDEO_LICENSE_API_KEY" },
  @{ Name = "caption-broker"; Secret = "CAPTION_BROKER_API_KEY"; EnvVar = "COREVIDEO_CAPTION_BROKER_API_KEY" },
  @{ Name = "telemetry-ingest"; Secret = "TELEMETRY_API_KEY"; EnvVar = "COREVIDEO_TELEMETRY_API_KEY" }
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
  if (-not $secretValue) {
    throw "Missing $($service.EnvVar) in $secretsFile"
  }

  Write-Host "Deploying $($service.Name) ..."
  Push-Location $serviceDir
  try {
    $secretValue | npx wrangler secret put $service.Secret
    npx wrangler deploy
  } finally {
    Pop-Location
  }
}

Write-Host "Staging workers deployed. Run: npm run smoke:staging-services"