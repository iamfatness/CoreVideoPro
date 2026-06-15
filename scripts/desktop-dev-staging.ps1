# Launch desktop shell with staging Cloudflare services wired in.
$ErrorActionPreference = "Stop"
. (Join-Path $PSScriptRoot "load-staging-env.ps1")
& (Join-Path $PSScriptRoot "desktop-dev.ps1") @args